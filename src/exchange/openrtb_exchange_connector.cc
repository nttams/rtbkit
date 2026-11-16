#include "exchange/openrtb_exchange_connector.h"

#include "agent/agent_config.h"
#include "common/openrtb_parser.h"
#include "exchange/mock_exchange_support.h"

#include <asio.hpp>
#include <llhttp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

namespace RTBKIT {

using nlohmann::json;

namespace {

/** What a creative's providerConfig.<exchange> says. */
struct CreativeData {
    std::string adm;
    std::string crid;
    std::vector<std::string> adomain;
};

/** Reads providerConfig.<exchange>; nullopt if it has bad types. */
std::optional<CreativeData> readCreativeData(const Creative& creative,
                                             const std::string& exchange)
{
    CreativeData data;
    if (!creative.providerConfig.is_object()) return data;
    auto it = creative.providerConfig.find(exchange);
    if (it == creative.providerConfig.end() || it->is_null()) return data;
    if (!it->is_object()) return std::nullopt;

    if (auto adm = it->find("adm"); adm != it->end() && !adm->is_null()) {
        if (!adm->is_string()) return std::nullopt;
        data.adm = adm->get<std::string>();
    }
    if (auto crid = it->find("crid"); crid != it->end() && !crid->is_null()) {
        if (crid->is_string()) data.crid = crid->get<std::string>();
        else if (crid->is_number_integer()) data.crid = std::to_string(crid->get<int64_t>());
        else return std::nullopt;
    }
    if (auto adv = it->find("adomain"); adv != it->end() && !adv->is_null()) {
        if (!adv->is_array()) return std::nullopt;
        for (const auto& d : *adv) {
            if (!d.is_string()) return std::nullopt;
            data.adomain.push_back(d.get<std::string>());
        }
    }
    return data;
}

template<typename T>
T setting(const json& config, const char* key, T fallback)
{
    auto it = config.find(key);
    if (it == config.end() || it->is_null()) return fallback;
    try {
        return it->get<T>();
    }
    catch (const json::exception&) {
        throw std::runtime_error(std::string("openrtb connector: bad value for \"")
                                 + key + "\"");
    }
}

json errorJson(const std::string& message) { return json{{"error", message}}; }

using tcp = asio::ip::tcp;

Auction::Clock::duration millis(double ms)
{
    return std::chrono::duration_cast<Auction::Clock::duration>(
        std::chrono::duration<double, std::milli>(ms));
}

const char* reasonPhrase(int status)
{
    switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    default: return "Unknown";
    }
}

/** Lets completion callbacks (router thread) post to a connection only while
    the event loop that owns it exists. */
struct Gate {
    std::mutex mutex;
    bool open = true;
};

} // namespace

/******************************************************************************/
/* SERVER: acceptor + event loop threads                                      */
/******************************************************************************/

struct OpenRtbExchangeConnector::Server {
    // Declared before ioc: sessions unregister while ioc destroys them.
    std::mutex mutex;   ///< guards sessions and stopping
    std::unordered_map<Session*, std::weak_ptr<Session>> sessions;
    bool stopping = false;
    std::shared_ptr<Gate> gate = std::make_shared<Gate>();

    asio::io_context ioc;
    tcp::acceptor acceptor{ioc};
    asio::steady_timer acceptRetry{ioc};
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work;
    std::vector<std::thread> threads;

    void accept(OpenRtbExchangeConnector& connector);
};

/******************************************************************************/
/* SESSION: one client connection                                             */
/******************************************************************************/

/** All of a session's handlers run on its strand (the socket's executor), so
    its state needs no lock. It reads one request, answers it, then reads the
    next; while its auction runs, no thread is held. */
class OpenRtbExchangeConnector::Session
    : public std::enable_shared_from_this<Session> {
public:
    Session(OpenRtbExchangeConnector& connector, Server& server, tcp::socket socket)
        : connector_(connector), server_(server), socket_(std::move(socket)),
          idleTimer_(socket_.get_executor()), deadlineTimer_(socket_.get_executor())
    {
        llhttp_settings_init(&settings_);
        settings_.on_message_begin = [](llhttp_t* p) { return self(p)->onMessageBegin(); };
        settings_.on_url = [](llhttp_t* p, const char* at, size_t n) {
            self(p)->url_.append(at, n);
            return 0;
        };
        settings_.on_headers_complete = [](llhttp_t* p) { return self(p)->onHeadersComplete(); };
        settings_.on_body = [](llhttp_t* p, const char* at, size_t n) {
            return self(p)->onBody(at, n);
        };
        settings_.on_message_complete = [](llhttp_t* p) {
            // Read while the message flags are still set; then stop parsing
            // until this request is answered (pipelined bytes stay buffered).
            self(p)->keepAlive_ = llhttp_should_keep_alive(p) != 0;
            return int(HPE_PAUSED);
        };
        llhttp_init(&parser_, HTTP_REQUEST, &settings_);
        parser_.data = this;
    }

    ~Session()
    {
        if (!registered_) return;
        std::lock_guard<std::mutex> lock(server_.mutex);
        server_.sessions.erase(this);
    }

    auto executor() { return socket_.get_executor(); }

    /** Runs on the strand. */
    void start()
    {
        {
            std::lock_guard<std::mutex> lock(server_.mutex);
            if (server_.stopping) {
                close();
                return;
            }
            server_.sessions.emplace(this, weak_from_this());
            registered_ = true;
        }
        asio::error_code ec;
        socket_.set_option(tcp::no_delay(true), ec);
        read();
    }

    /** Runs on the strand. Idle connections close now; one with a request
        in progress closes after its answer. */
    void beginShutdown()
    {
        stopping_ = true;
        if (!busy_) close();
    }

private:
    static Session* self(llhttp_t* p) { return static_cast<Session*>(p->data); }

    int onMessageBegin()
    {
        // Like the original connector, the auction clock starts when the
        // first byte of the request was read.
        firstData_ = chunkTime_;
        url_.clear();
        body_.clear();
        return 0;
    }

    int onHeadersComplete()
    {
        method_ = llhttp_method_t(parser_.method);
        if (parser_.content_length > connector_.maxBodyBytes_
            && parser_.content_length != uint64_t(-1)) {
            tooLarge_ = true;
            return -1;
        }
        return 0;
    }

    int onBody(const char* at, size_t n)
    {
        if (body_.size() + n > connector_.maxBodyBytes_) {
            tooLarge_ = true;
            return -1;
        }
        body_.append(at, n);
        return 0;
    }

    void armIdle()
    {
        idleTimer_.expires_after(millis(connector_.idleTimeoutMs_));
        idleTimer_.async_wait([self = shared_from_this()](asio::error_code ec) {
            if (!ec && self->idleTimer_.expiry() <= Auction::Clock::now()) self->close();
        });
    }

    void read()
    {
        if (stopping_ || closed_) {
            close();
            return;
        }
        armIdle();
        socket_.async_read_some(asio::buffer(readBuffer_),
            [self = shared_from_this()](asio::error_code ec, size_t n) {
                self->onRead(ec, n);
            });
    }

    void onRead(asio::error_code ec, size_t n)
    {
        if (ec) {
            close();
            return;
        }
        chunkTime_ = Auction::Clock::now();
        pending_.append(readBuffer_.data(), n);
        parse();
    }

    /** Feeds the buffered bytes to the parser until a request is complete. */
    void parse()
    {
        if (offset_ >= pending_.size()) {
            pending_.clear();
            offset_ = 0;
            read();
            return;
        }
        const char* begin = pending_.data() + offset_;
        llhttp_errno_t err = llhttp_execute(&parser_, begin, pending_.size() - offset_);
        if (err == HPE_OK) {
            pending_.clear();
            offset_ = 0;
            read();
            return;
        }
        if (err == HPE_PAUSED) {
            offset_ = size_t(llhttp_get_error_pos(&parser_) - pending_.data());
            idleTimer_.cancel();
            busy_ = true;
            dispatch();
            return;
        }
        busy_ = true;
        keepAlive_ = false;
        if (tooLarge_) {
            connector_.badRequests_++;
            respond(413, errorJson("request body too large").dump());
        }
        else {
            connector_.badRequests_++;
            respond(400, errorJson(std::string("bad HTTP request: ")
                                   + llhttp_errno_name(err)).dump());
        }
    }

    void dispatch()
    {
        const std::string path = url_.substr(0, url_.find('?'));
        try {
            if (path == "/auctions" && method_ == HTTP_POST) startAuction();
            else if (path == "/ping" && method_ == HTTP_GET) respond(200, "pong", "text/plain");
            else respond(404, errorJson("not found").dump());
        }
        catch (const std::exception& e) {
            connector_.errors_++;
            auction_.reset();
            respond(500, errorJson(e.what()).dump());
        }
    }

    void startAuction()
    {
        const uint64_t id = ++requestId_;
        auto onFinished = [weak = weak_from_this(), gate = server_.gate, id](
                std::shared_ptr<Auction>) {
            // Router thread: only hand the result over to the connection.
            std::lock_guard<std::mutex> lock(gate->mutex);
            if (!gate->open) return;
            if (auto s = weak.lock())
                asio::post(s->executor(), [s, id] { s->auctionDone(id, false); });
        };

        int status = 500;
        std::string body;
        auto auction = connector_.beginAuction(body_, firstData_, std::move(onFinished),
                                               status, body);
        if (!auction) {
            respond(status, body);
            return;
        }
        auction_ = std::move(auction);
        deadlineTimer_.expires_at(auction_->expiry() + millis(connector_.graceMs_));
        deadlineTimer_.async_wait([self = shared_from_this(), id](asio::error_code ec) {
            if (!ec) self->auctionDone(id, true);
        });
    }

    /** The auction finished (router callback) or its deadline passed. */
    void auctionDone(uint64_t id, bool deadline)
    {
        if (id != requestId_ || !auction_) return;   // already answered
        auto auction = std::move(auction_);
        auction_.reset();
        // At the deadline, finish() closes the auction so late bids are
        // refused; false means the router finished it just in time.
        const bool timedOut = deadline && auction->finish();
        if (!deadline) deadlineTimer_.cancel();

        int status = 500;
        std::string body;
        connector_.endAuction(*auction, timedOut, status, body);
        respond(status, body);
    }

    void respond(int status, const std::string& body,
                 const char* contentType = "application/json")
    {
        const bool keepAlive = keepAlive_ && !stopping_;
        writeBuffer_ = "HTTP/1.1 " + std::to_string(status) + " " + reasonPhrase(status) + "\r\n";
        if (status != 204) {
            writeBuffer_ += std::string("Content-Type: ") + contentType + "\r\n";
            writeBuffer_ += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        }
        writeBuffer_ += keepAlive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
        if (status != 204) writeBuffer_ += body;
        keepAlive_ = keepAlive;

        armIdle();
        asio::async_write(socket_, asio::buffer(writeBuffer_),
            [self = shared_from_this()](asio::error_code ec, size_t) {
                self->onWritten(ec);
            });
    }

    void onWritten(asio::error_code ec)
    {
        idleTimer_.cancel();
        busy_ = false;
        if (ec || !keepAlive_ || stopping_) {
            close();
            return;
        }
        url_.clear();
        body_.clear();
        llhttp_resume(&parser_);
        parse();   // a pipelined request may already be buffered
    }

    void close()
    {
        if (closed_) return;
        closed_ = true;
        asio::error_code ec;
        socket_.shutdown(tcp::socket::shutdown_both, ec);
        socket_.close(ec);
        idleTimer_.cancel();
        deadlineTimer_.cancel();
    }

    OpenRtbExchangeConnector& connector_;
    Server& server_;
    tcp::socket socket_;
    asio::steady_timer idleTimer_;
    asio::steady_timer deadlineTimer_;

    llhttp_settings_t settings_;
    llhttp_t parser_;
    std::array<char, 16384> readBuffer_;
    std::string pending_;     ///< bytes read and not yet parsed from offset_
    size_t offset_ = 0;
    std::string writeBuffer_;

    // Current request.
    Auction::TimePoint chunkTime_{};
    Auction::TimePoint firstData_{};
    std::string url_;
    std::string body_;
    llhttp_method_t method_ = HTTP_GET;
    bool keepAlive_ = false;
    bool tooLarge_ = false;
    uint64_t requestId_ = 0;
    std::shared_ptr<Auction> auction_;

    bool busy_ = false;       ///< a request is being answered
    bool stopping_ = false;
    bool closed_ = false;
    bool registered_ = false;
};

void OpenRtbExchangeConnector::Server::accept(OpenRtbExchangeConnector& connector)
{
    acceptor.async_accept(asio::make_strand(ioc),
        [this, &connector](asio::error_code ec, tcp::socket socket) {
            if (ec == asio::error::operation_aborted || !acceptor.is_open()) return;
            if (ec) {
                // E.g. out of file descriptors: try again shortly.
                acceptRetry.expires_after(std::chrono::milliseconds(10));
                acceptRetry.async_wait([this, &connector](asio::error_code e) {
                    if (!e) accept(connector);
                });
                return;
            }
            auto session = std::make_shared<Session>(connector, *this, std::move(socket));
            asio::dispatch(session->executor(), [session] { session->start(); });
            accept(connector);
        });
}

/******************************************************************************/
/* CONNECTOR                                                                  */
/******************************************************************************/

OpenRtbExchangeConnector::OpenRtbExchangeConnector(std::string name)
    : ExchangeConnector(std::move(name))
{
}

OpenRtbExchangeConnector::~OpenRtbExchangeConnector()
{
    stop();
}

void OpenRtbExchangeConnector::configure(const json& config)
{
    if (!config.is_object() && !config.is_null())
        throw std::runtime_error("openrtb connector: config must be an object");

    std::string name = setting<std::string>(config, "name", exchangeName());
    if (name.empty()) throw std::runtime_error("openrtb connector: empty name");
    host_ = setting<std::string>(config, "host", host_);
    int port = setting<int>(config, "port", configuredPort_);
    if (port < 0 || port > 65535)
        throw std::runtime_error("openrtb connector: bad port");
    int threads = setting<int>(config, "threads", threads_);
    if (threads < 1) throw std::runtime_error("openrtb connector: threads must be >= 1");
    std::string winUrl = setting<std::string>(config, "winNoticeUrl", winNoticeUrl_);
    if (winUrl.empty()) throw std::runtime_error("openrtb connector: empty winNoticeUrl");
    double maxTmax = setting<double>(config, "maxTmaxMs", maxTmaxMs_);
    double grace = setting<double>(config, "graceMs", graceMs_);
    if (maxTmax <= 0 || grace < 0)
        throw std::runtime_error("openrtb connector: bad maxTmaxMs or graceMs");
    double idle = setting<double>(config, "idleTimeoutMs", idleTimeoutMs_);
    if (idle <= 0) throw std::runtime_error("openrtb connector: bad idleTimeoutMs");
    int64_t maxBody = setting<int64_t>(config, "maxBodyBytes", int64_t(maxBodyBytes_));
    if (maxBody < 1) throw std::runtime_error("openrtb connector: bad maxBodyBytes");

    setExchangeName(name);
    configuredPort_ = port;
    threads_ = threads;
    winNoticeUrl_ = winUrl;
    maxTmaxMs_ = maxTmax;
    graceMs_ = grace;
    idleTimeoutMs_ = idle;
    maxBodyBytes_ = size_t(maxBody);
}

void OpenRtbExchangeConnector::start()
{
    if (server_) return;

    auto server = std::make_shared<Server>();
    const std::string where = host_ + ":" + std::to_string(configuredPort_);

    asio::error_code ec;
    tcp::endpoint endpoint;
    auto address = asio::ip::make_address(host_, ec);
    if (!ec) {
        endpoint = tcp::endpoint(address, uint16_t(configuredPort_));
    }
    else {
        tcp::resolver resolver(server->ioc);
        auto results = resolver.resolve(host_, std::to_string(configuredPort_), ec);
        if (ec || results.empty())
            throw std::runtime_error("openrtb connector: can not resolve " + where);
        endpoint = *results.begin();
    }

    auto& acceptor = server->acceptor;
    acceptor.open(endpoint.protocol(), ec);
    if (!ec) acceptor.set_option(tcp::acceptor::reuse_address(true), ec);
    if (!ec) acceptor.bind(endpoint, ec);
    if (!ec) acceptor.listen(asio::socket_base::max_listen_connections, ec);
    if (ec)
        throw std::runtime_error("openrtb connector: can not listen on " + where
                                 + ": " + ec.message());

    port_ = acceptor.local_endpoint().port();
    server->work.emplace(asio::make_work_guard(server->ioc));
    server->accept(*this);
    for (int i = 0; i < threads_; ++i) {
        server->threads.emplace_back([s = server.get()] {
            while (true) {
                try {
                    s->ioc.run();
                    return;
                }
                catch (const std::exception& e) {
                    std::cerr << "openrtb connector: event loop error: " << e.what()
                              << std::endl;
                }
            }
        });
    }
    server_ = std::move(server);
}

void OpenRtbExchangeConnector::stop()
{
    if (!server_) return;
    Server& s = *server_;

    // No new connections; idle ones close; a request in progress still gets
    // its answer (at the latest at its deadline), then its connection closes.
    std::vector<std::shared_ptr<Session>> sessions;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.stopping = true;
        for (auto& [ptr, weak] : s.sessions)
            if (auto session = weak.lock()) sessions.push_back(std::move(session));
    }
    asio::post(s.ioc, [&s] {
        asio::error_code ec;
        s.acceptor.close(ec);
        s.acceptRetry.cancel();
    });
    for (auto& session : sessions)
        asio::post(session->executor(), [session] { session->beginShutdown(); });
    sessions.clear();

    s.work.reset();
    for (auto& t : s.threads) t.join();
    {
        std::lock_guard<std::mutex> lock(s.gate->mutex);
        s.gate->open = false;
    }
    server_.reset();
    port_ = 0;
}

OpenRtbExchangeConnector::Stats OpenRtbExchangeConnector::stats() const
{
    Stats s;
    s.requests = requests_;
    s.bids = bids_;
    s.noBids = noBids_;
    s.timeouts = timeouts_;
    s.expiredInQueue = expiredInQueue_;
    s.badRequests = badRequests_;
    s.errors = errors_;
    return s;
}

std::shared_ptr<void> OpenRtbExchangeConnector::getCreativeProviderData(
        const AgentConfig&, const Creative& creative) const
{
    auto data = readCreativeData(creative, exchangeName());
    if (!data) return nullptr;
    return std::make_shared<CreativeData>(std::move(*data));
}

std::shared_ptr<Auction> OpenRtbExchangeConnector::beginAuction(
        const std::string& body, Auction::TimePoint firstData,
        Auction::Completion onFinished, int& status, std::string& responseBody)
{
    ++requests_;

    std::shared_ptr<BidRequest> request;
    try {
        OpenRtbOptions options;
        options.exchange = exchangeName();
        options.provider = exchangeName();
        request = std::make_shared<BidRequest>(parseOpenRtbBidRequestText(body, options));
    }
    catch (const std::exception& e) {
        ++badRequests_;
        status = 400;
        responseBody = errorJson(e.what()).dump();
        return nullptr;
    }

    request->timeAvailableMs = std::clamp(request->timeAvailableMs, 1.0, maxTmaxMs_);

    // The exchange started its clock when it sent the request: if its tmax
    // was over by the time the whole request was read, nobody is waiting for
    // the answer any more.
    if (Auction::Clock::now() >= firstData + millis(request->timeAvailableMs)) {
        ++expiredInQueue_;
        ++noBids_;
        status = 204;
        return nullptr;
    }
    if (request->timestamp == Date()) request->timestamp = std::chrono::system_clock::now();

    auto auction = Auction::create(request, std::move(onFinished), firstData);
    try {
        submit(auction);
    }
    catch (const std::exception& e) {
        ++errors_;
        auction->finish();
        status = 500;
        responseBody = errorJson(e.what()).dump();
        return nullptr;
    }
    return auction;
}

void OpenRtbExchangeConnector::endAuction(const Auction& auction, bool timedOut,
                                          int& status, std::string& responseBody)
{
    responseBody.clear();
    if (timedOut) {
        // The router missed the deadline: answer no bid.
        ++timeouts_;
    }
    else {
        try {
            responseBody = buildBidResponse(auction);
        }
        catch (const std::exception& e) {
            ++errors_;
            status = 500;
            responseBody = errorJson(e.what()).dump();
            return;
        }
    }

    if (responseBody.empty()) {
        ++noBids_;
        status = 204;
    }
    else {
        ++bids_;
        status = 200;
    }
}

std::string OpenRtbExchangeConnector::buildBidResponse(const Auction& auction) const
{
    const BidRequest& request = auction.request();
    const std::string exchange = exchangeName();

    json bids = json::array();
    std::string currency;

    for (size_t spot = 0; spot < auction.numSpots(); ++spot) {
        auto best = auction.bestResponse(spot);
        if (!best || !best->valid() || best->price <= Amount()) continue;
        if (size_t(best->spotIndex) >= request.imp.size()) continue;

        const std::string& impId = request.imp[best->spotIndex].id.toString();

        json bid;
        bid["id"] = auction.id().toString() + "-" + impId;
        bid["impid"] = impId;
        bid["price"] = best->price.toCpm();
        bid["nurl"] = winNoticeUrl_ + "?auctionId=" + urlEncode(auction.id().toString())
            + "&adSpotId=" + urlEncode(impId) + "&price=${AUCTION_PRICE}";
        if (!best->agent.empty()) bid["cid"] = best->agent;

        std::string crid = std::to_string(best->creativeIndex);
        if (best->config && size_t(best->creativeIndex) < best->config->creatives.size()) {
            const Creative& creative = best->config->creatives[best->creativeIndex];
            if (creative.id >= 0) crid = std::to_string(creative.id);
            if (creative.format.valid()) {
                bid["w"] = creative.format.width;
                bid["h"] = creative.format.height;
            }

            std::optional<CreativeData> data;
            auto it = creative.providerData.find(exchange);
            if (it != creative.providerData.end() && it->second)
                data = *static_cast<const CreativeData*>(it->second.get());
            else
                data = readCreativeData(creative, exchange);
            if (data) {
                if (!data->crid.empty()) crid = data->crid;
                if (!data->adm.empty()) bid["adm"] = data->adm;
                if (!data->adomain.empty()) bid["adomain"] = data->adomain;
            }
        }
        bid["crid"] = crid;

        if (currency.empty()) currency = toString(best->price.currencyCode);
        bids.push_back(std::move(bid));
    }

    if (bids.empty()) return {};

    json response;
    response["id"] = auction.id().toString();
    response["cur"] = currency;
    response["seatbid"] = json::array({json{{"bid", std::move(bids)}}});
    return response.dump();
}

} // namespace RTBKIT

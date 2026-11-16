#include "exchange/openrtb_exchange_connector.h"

#include "agent/agent_config.h"
#include "common/openrtb_parser.h"
#include "exchange/mock_exchange_support.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <stdexcept>

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

Auction::Clock::duration millis(double ms)
{
    return std::chrono::duration_cast<Auction::Clock::duration>(
        std::chrono::duration<double, std::milli>(ms));
}

} // namespace

/** One request waiting for its auction: answered once, by whichever comes
    first of the router's completion and the deadline. */
struct OpenRtbExchangeConnector::Pending {
    explicit Pending(HttpResponder r) : responder(std::move(r)) {}
    std::atomic<bool> answered{false};
    HttpResponder responder;
};

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
    if (server_ && server_->running()) return;

    auto server = std::make_shared<HttpServer>();
    server->route("POST", "/auctions", [this](const HttpRequest& request, HttpResponder r) {
        onAuctionRequest(request, std::move(r));
    });
    server->route("GET", "/ping", [](const HttpRequest&, HttpResponder r) {
        r.send({200, "pong", "text/plain"});
    });

    HttpServer::Options options;
    options.host = host_;
    options.port = configuredPort_;
    options.threads = threads_;
    options.idleTimeoutMs = idleTimeoutMs_;
    options.maxBodyBytes = maxBodyBytes_;
    // Every request in progress is answered by its deadline.
    options.drainTimeoutMs = maxTmaxMs_ + graceMs_ + 500;
    options.name = "openrtb connector";
    server->start(options);

    port_ = server->port();
    server_ = std::move(server);
}

void OpenRtbExchangeConnector::stop()
{
    if (!server_) return;
    // No new connections; a request in progress still gets its answer (at
    // the latest at its deadline), then the threads join.
    server_->stop();
    server_.reset();
    port_ = 0;
}

void OpenRtbExchangeConnector::onAuctionRequest(const HttpRequest& httpRequest,
                                                HttpResponder responder)
{
    auto pending = std::make_shared<Pending>(std::move(responder));
    std::weak_ptr<HttpServer> server = server_;

    // Runs on the router thread: only hand the auction back to an event loop
    // thread, which builds and sends the answer.
    auto onFinished = [this, server, pending](std::shared_ptr<Auction> auction) {
        if (auto s = server.lock())
            s->post([this, pending, auction] { answer(*pending, *auction, false); });
    };

    int status = 500;
    std::string body;
    auto auction = beginAuction(httpRequest.body, httpRequest.firstData,
                                std::move(onFinished), status, body);
    if (!auction) {
        pending->answered = true;
        pending->responder.send({status, body, "application/json"});
        return;
    }
    try {
        submit(auction);
    }
    catch (const std::exception& e) {
        ++errors_;
        pending->answered = true;
        pending->responder.send({500, errorJson(e.what()).dump(), "application/json"});
        auction->finish();   // its completion now does nothing
        return;
    }
    server_->runAt(auction->expiry() + millis(graceMs_), [this, pending, auction] {
        answer(*pending, *auction, true);
    });
}

void OpenRtbExchangeConnector::answer(Pending& pending, Auction& auction, bool deadline)
{
    if (pending.answered.exchange(true)) return;
    // At the deadline, finish() closes the auction so late bids are refused;
    // false means the router finished it just in time.
    const bool timedOut = deadline && auction.finish();
    int status = 500;
    std::string body;
    endAuction(auction, timedOut, status, body);
    pending.responder.send({status, std::move(body), "application/json"});
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

    return Auction::create(request, std::move(onFinished), firstData);
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

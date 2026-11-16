#include "http/http_server.h"

#include <asio.hpp>
#include <llhttp.h>

#include <array>
#include <atomic>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

namespace RTBKIT {

using tcp = asio::ip::tcp;
using Clock = HttpRequest::Clock;

namespace {

Clock::duration millis(double ms)
{
    return std::chrono::duration_cast<Clock::duration>(
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
    case 503: return "Service Unavailable";
    default: return "Unknown";
    }
}

int hexValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string urlDecode(std::string_view in)
{
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == '+') out += ' ';
        else if (c == '%' && i + 2 < in.size()
                 && hexValue(in[i + 1]) >= 0 && hexValue(in[i + 2]) >= 0) {
            out += char(hexValue(in[i + 1]) * 16 + hexValue(in[i + 2]));
            i += 2;
        }
        else out += c;
    }
    return out;
}

} // namespace

std::optional<std::string> HttpRequest::param(const std::string& name) const
{
    std::string_view rest = query;
    while (!rest.empty()) {
        size_t amp = rest.find('&');
        std::string_view pair = rest.substr(0, amp);
        rest = amp == std::string_view::npos ? std::string_view() : rest.substr(amp + 1);
        size_t eq = pair.find('=');
        std::string key = urlDecode(pair.substr(0, eq));
        if (key != name) continue;
        return eq == std::string_view::npos ? std::string() : urlDecode(pair.substr(eq + 1));
    }
    return std::nullopt;
}

/******************************************************************************/
/* SHARED STATE                                                               */
/******************************************************************************/

/** Lets other threads reach the event loop only while it exists. One per
    HttpServer, for its whole life (start / stop / start reuse it). */
struct Gate {
    std::mutex mutex;
    HttpServer::Loop* loop = nullptr;   ///< null when stopped
};

struct HttpServer::Routes {
    std::map<std::string, HttpHandler> handlers;   ///< "METHOD path"
    std::shared_ptr<Gate> gate = std::make_shared<Gate>();
};

class Session;

struct HttpServer::Loop {
    explicit Loop(Options o, std::shared_ptr<Routes> r)
        : options(std::move(o)), routes(std::move(r)) {}

    // Declared before ioc: sessions unregister while ioc destroys them.
    std::mutex mutex;   ///< guards the fields below
    std::unordered_map<Session*, std::weak_ptr<Session>> sessions;
    std::vector<std::weak_ptr<asio::steady_timer>> timers;
    bool stopping = false;
    bool drained = false;      ///< finishDrain ran: no new timers
    bool destroying = false;

    Options options;
    std::shared_ptr<Routes> routes;

    asio::io_context ioc;
    tcp::acceptor acceptor{ioc};
    asio::steady_timer acceptRetry{ioc};
    asio::steady_timer drainTimer{ioc};
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work;
    std::vector<std::thread> threads;

    void accept();
    void sessionGone(Session* session);
    void finishDrain();
};

struct HttpResponder::State {
    std::weak_ptr<Session> session;
    std::shared_ptr<Gate> gate;
    uint64_t requestId = 0;
    std::atomic<bool> sent{false};

    void deliver(HttpResponse response);
    ~State()
    {
        // A handler that forgot to answer must not leave the client hanging.
        if (!sent.exchange(true)) deliverNow({500, "no response", "text/plain"});
    }

private:
    void deliverNow(HttpResponse response);
};

/******************************************************************************/
/* SESSION: one client connection                                             */
/******************************************************************************/

/** All of a session's handlers run on its strand (the socket's executor), so
    its state needs no lock. It reads one request, answers it, then reads the
    next; while a handler works on the answer, no thread is held. */
class Session : public std::enable_shared_from_this<Session> {
public:
    Session(HttpServer::Loop& loop, tcp::socket socket)
        : loop_(loop), socket_(std::move(socket)), idleTimer_(socket_.get_executor())
    {
        llhttp_settings_init(&settings_);
        settings_.on_message_begin = [](llhttp_t* p) { return self(p)->onMessageBegin(); };
        settings_.on_url = [](llhttp_t* p, const char* at, size_t n) {
            self(p)->request_.target.append(at, n);
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
        if (registered_) loop_.sessionGone(this);
    }

    auto executor() { return socket_.get_executor(); }

    /** On the strand. */
    void start()
    {
        {
            std::lock_guard<std::mutex> lock(loop_.mutex);
            if (loop_.stopping) {
                close();
                return;
            }
            loop_.sessions.emplace(this, weak_from_this());
            registered_ = true;
        }
        asio::error_code ec;
        socket_.set_option(tcp::no_delay(true), ec);
        read();
    }

    /** On the strand. Idle connections close now; one with a request in
        progress closes after its answer. */
    void beginShutdown()
    {
        stopping_ = true;
        if (!busy_) close();
    }

    /** On the strand: the drain timeout passed. */
    void forceClose()
    {
        stopping_ = true;
        close();
    }

    /** On the strand: the answer for request `id`. */
    void respond(uint64_t id, HttpResponse response)
    {
        if (id != requestId_ || !awaiting_ || closed_) return;
        awaiting_ = false;
        write(std::move(response));
    }

private:
    static Session* self(llhttp_t* p) { return static_cast<Session*>(p->data); }

    int onMessageBegin()
    {
        request_ = HttpRequest();
        request_.firstData = chunkTime_;
        return 0;
    }

    int onHeadersComplete()
    {
        request_.method = llhttp_method_name(llhttp_method_t(parser_.method));
        if (parser_.content_length != uint64_t(-1)
            && parser_.content_length > loop_.options.maxBodyBytes) {
            tooLarge_ = true;
            return -1;
        }
        return 0;
    }

    int onBody(const char* at, size_t n)
    {
        if (request_.body.size() + n > loop_.options.maxBodyBytes) {
            tooLarge_ = true;
            return -1;
        }
        request_.body.append(at, n);
        return 0;
    }

    void armIdle()
    {
        idleTimer_.expires_after(millis(loop_.options.idleTimeoutMs));
        idleTimer_.async_wait([self = shared_from_this()](asio::error_code ec) {
            if (!ec && self->idleTimer_.expiry() <= Clock::now()) self->close();
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
        chunkTime_ = Clock::now();
        pending_.append(readBuffer_.data(), n);
        parse();
    }

    /** Feeds buffered bytes to the parser until a request is complete. */
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
        busy_ = true;
        idleTimer_.cancel();
        if (err == HPE_PAUSED) {
            offset_ = size_t(llhttp_get_error_pos(&parser_) - pending_.data());
            dispatch();
            return;
        }
        keepAlive_ = false;
        if (tooLarge_) write({413, "request body too large"});
        else write({400, std::string("bad HTTP request: ") + llhttp_errno_name(err)});
    }

    void dispatch()
    {
        HttpRequest& r = request_;
        size_t q = r.target.find('?');
        r.path = r.target.substr(0, q);
        r.query = q == std::string::npos ? std::string() : r.target.substr(q + 1);

        auto it = loop_.routes->handlers.find(r.method + " " + r.path);
        if (it == loop_.routes->handlers.end()) {
            write({404, "not found"});
            return;
        }

        auto state = std::make_shared<HttpResponder::State>();
        state->session = weak_from_this();
        state->gate = loop_.routes->gate;
        state->requestId = ++requestId_;
        awaiting_ = true;
        // No I/O is pending while the handler works on the answer: hold
        // ourselves (the responder only has a weak pointer) and keep the loop
        // running, so that stop() waits for the answer.
        holdSelf_ = shared_from_this();
        work_.emplace(loop_.ioc.get_executor());
        try {
            it->second(r, HttpResponder(std::move(state)));
        }
        catch (const std::exception& e) {
            std::cerr << loop_.options.name << ": handler error: " << e.what() << std::endl;
            respond(requestId_, {500, e.what()});
        }
    }

    void write(HttpResponse response)
    {
        work_.reset();
        auto self = std::move(holdSelf_);
        const bool keepAlive = keepAlive_ && !stopping_;
        const int status = response.status;
        writeBuffer_ = "HTTP/1.1 " + std::to_string(status) + " " + reasonPhrase(status) + "\r\n";
        if (status != 204 && status != 304 && status >= 200) {
            writeBuffer_ += "Content-Type: " + response.contentType + "\r\n";
            writeBuffer_ += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
        }
        writeBuffer_ += keepAlive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
        if (status != 204 && status != 304) writeBuffer_ += response.body;
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
        llhttp_resume(&parser_);
        parse();   // a pipelined request may already be buffered
    }

    void close()
    {
        work_.reset();
        auto self = std::move(holdSelf_);
        if (closed_) return;
        closed_ = true;
        asio::error_code ec;
        socket_.shutdown(tcp::socket::shutdown_both, ec);
        socket_.close(ec);
        idleTimer_.cancel();
    }

    HttpServer::Loop& loop_;
    tcp::socket socket_;
    asio::steady_timer idleTimer_;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work_;
    std::shared_ptr<Session> holdSelf_;   ///< set while a handler owes the answer

    llhttp_settings_t settings_;
    llhttp_t parser_;
    std::array<char, 16384> readBuffer_;
    std::string pending_;     ///< bytes read, not parsed before offset_
    size_t offset_ = 0;
    std::string writeBuffer_;

    Clock::time_point chunkTime_{};
    HttpRequest request_;
    bool keepAlive_ = false;
    bool tooLarge_ = false;
    uint64_t requestId_ = 0;

    bool awaiting_ = false;   ///< a handler owes the answer
    bool busy_ = false;       ///< a request is being answered
    bool stopping_ = false;
    bool closed_ = false;
    bool registered_ = false;
};

/******************************************************************************/
/* RESPONDER                                                                  */
/******************************************************************************/

void HttpResponder::send(HttpResponse response) const
{
    if (!state_ || state_->sent.exchange(true)) return;
    state_->deliver(std::move(response));
}

void HttpResponder::State::deliver(HttpResponse response) { deliverNow(std::move(response)); }

void HttpResponder::State::deliverNow(HttpResponse response)
{
    std::lock_guard<std::mutex> lock(gate->mutex);
    if (!gate->loop) return;
    if (auto s = session.lock()) {
        asio::post(s->executor(), [s, id = requestId, r = std::move(response)]() mutable {
            s->respond(id, std::move(r));
        });
    }
}

/******************************************************************************/
/* LOOP                                                                       */
/******************************************************************************/

void HttpServer::Loop::accept()
{
    acceptor.async_accept(asio::make_strand(ioc),
        [this](asio::error_code ec, tcp::socket socket) {
            if (ec == asio::error::operation_aborted || !acceptor.is_open()) return;
            if (ec) {
                // E.g. out of file descriptors: try again shortly.
                acceptRetry.expires_after(std::chrono::milliseconds(10));
                acceptRetry.async_wait([this](asio::error_code e) {
                    if (!e) accept();
                });
                return;
            }
            auto session = std::make_shared<Session>(*this, std::move(socket));
            asio::dispatch(session->executor(), [session] { session->start(); });
            accept();
        });
}

void HttpServer::Loop::sessionGone(Session* session)
{
    std::lock_guard<std::mutex> lock(mutex);
    sessions.erase(session);
    if (stopping && !destroying && sessions.empty())
        asio::post(ioc, [this] { finishDrain(); });
}

/** Everything answered (or the drain timeout passed): let the threads end. */
void HttpServer::Loop::finishDrain()
{
    drainTimer.cancel();
    acceptRetry.cancel();
    std::vector<std::shared_ptr<Session>> left;
    std::vector<std::shared_ptr<asio::steady_timer>> pending;
    {
        std::lock_guard<std::mutex> lock(mutex);
        drained = true;
        for (auto& [ptr, weak] : sessions)
            if (auto s = weak.lock()) left.push_back(std::move(s));
        for (auto& weak : timers)
            if (auto t = weak.lock()) pending.push_back(std::move(t));
        timers.clear();
    }
    for (auto& s : left) asio::post(s->executor(), [s] { s->forceClose(); });
    for (auto& t : pending) t->cancel();
}

/******************************************************************************/
/* SERVER                                                                     */
/******************************************************************************/

HttpServer::HttpServer() : routes_(std::make_shared<Routes>()) {}

HttpServer::~HttpServer() { stop(); }

void HttpServer::route(std::string method, std::string path, HttpHandler handler)
{
    if (loop_) throw std::logic_error("HttpServer::route after start");
    routes_->handlers[method + " " + path] = std::move(handler);
}

void HttpServer::start(const Options& options)
{
    if (loop_) return;
    if (options.threads < 1) throw std::runtime_error(options.name + ": threads must be >= 1");

    auto loop = std::make_shared<Loop>(options, routes_);
    const std::string where = options.host + ":" + std::to_string(options.port);

    asio::error_code ec;
    tcp::endpoint endpoint;
    auto address = asio::ip::make_address(options.host, ec);
    if (!ec) {
        endpoint = tcp::endpoint(address, uint16_t(options.port));
    }
    else {
        tcp::resolver resolver(loop->ioc);
        auto results = resolver.resolve(options.host, std::to_string(options.port), ec);
        if (ec || results.empty())
            throw std::runtime_error(options.name + ": can not resolve " + where);
        endpoint = *results.begin();
    }

    auto& acceptor = loop->acceptor;
    acceptor.open(endpoint.protocol(), ec);
    // Only SO_REUSEADDR (quick restarts), never SO_REUSEPORT: two servers must
    // not share a port silently.
    if (!ec) acceptor.set_option(tcp::acceptor::reuse_address(true), ec);
    if (!ec) acceptor.bind(endpoint, ec);
    if (!ec) acceptor.listen(asio::socket_base::max_listen_connections, ec);
    if (ec)
        throw std::runtime_error(options.name + ": can not listen on " + where + ": "
                                 + ec.message());

    port_ = acceptor.local_endpoint().port();
    loop->work.emplace(loop->ioc.get_executor());
    loop->accept();
    {
        std::lock_guard<std::mutex> lock(routes_->gate->mutex);
        routes_->gate->loop = loop.get();
    }
    for (int i = 0; i < options.threads; ++i) {
        loop->threads.emplace_back([l = loop.get()] {
            while (true) {
                try {
                    l->ioc.run();
                    return;
                }
                catch (const std::exception& e) {
                    std::cerr << l->options.name << ": event loop error: " << e.what()
                              << std::endl;
                }
            }
        });
    }
    loop_ = std::move(loop);
}

void HttpServer::stop()
{
    if (!loop_) return;
    Loop& l = *loop_;

    std::vector<std::shared_ptr<Session>> sessions;
    {
        std::lock_guard<std::mutex> lock(l.mutex);
        l.stopping = true;
        for (auto& [ptr, weak] : l.sessions)
            if (auto s = weak.lock()) sessions.push_back(std::move(s));
    }
    const bool none = sessions.empty();
    asio::post(l.ioc, [&l, none] {
        asio::error_code ec;
        l.acceptor.close(ec);
        if (none) {
            l.finishDrain();
            return;
        }
        l.drainTimer.expires_after(millis(l.options.drainTimeoutMs));
        l.drainTimer.async_wait([&l](asio::error_code e) {
            if (!e) l.finishDrain();
        });
    });
    for (auto& s : sessions) asio::post(s->executor(), [s] { s->beginShutdown(); });
    sessions.clear();

    l.work.reset();
    for (auto& t : l.threads) t.join();
    {
        std::lock_guard<std::mutex> lock(routes_->gate->mutex);
        routes_->gate->loop = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(l.mutex);
        l.destroying = true;
    }
    loop_.reset();
    port_ = 0;
}

void HttpServer::post(std::function<void()> fn) const
{
    std::lock_guard<std::mutex> lock(routes_->gate->mutex);
    if (routes_->gate->loop) asio::post(routes_->gate->loop->ioc, std::move(fn));
}

void HttpServer::runAt(Clock::time_point when, std::function<void()> fn) const
{
    std::lock_guard<std::mutex> lock(routes_->gate->mutex);
    Loop* loop = routes_->gate->loop;
    if (!loop) return;
    auto timer = std::make_shared<asio::steady_timer>(loop->ioc, when);
    {
        // Tracked so that stop() can cancel it once every answer is out.
        std::lock_guard<std::mutex> l(loop->mutex);
        if (loop->drained) return;
        std::erase_if(loop->timers, [](auto& w) { return w.expired(); });
        loop->timers.push_back(timer);
    }
    timer->async_wait([timer, fn = std::move(fn)](asio::error_code ec) {
        if (!ec) fn();
    });
}

} // namespace RTBKIT

// Asynchronous HTTP/1.1 server (asio + llhttp), the modern counterpart of the
// original soa HttpEndpoint / PassiveEndpoint: a few event loop threads serve
// every connection, and a handler may answer later from any thread, so no
// thread waits for slow work (an auction, say).
//
//   HttpServer server;
//   server.route("GET", "/ping", [](const HttpRequest&, HttpResponder r) {
//       r.send({200, "pong"});
//   });
//   server.start({.host = "127.0.0.1", .port = 0, .threads = 2});
//
// A connection reads one request, waits for its answer, then reads the next
// (keep-alive and pipelining work, answers stay in order). Unknown routes are
// 404, malformed HTTP is 400 and a body over maxBodyBytes is 413; those close
// the connection. A connection idle (or sending) for idleTimeoutMs closes.
//
// stop(): no new connections, idle ones close, a request in progress still
// gets its answer (up to drainTimeoutMs), then the threads join. After stop()
// HttpResponder::send, post and runAt are no-ops.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace RTBKIT {

struct HttpRequest {
    using Clock = std::chrono::steady_clock;

    std::string method;   ///< "GET", "POST"...
    std::string target;   ///< as sent, e.g. "/win?auctionId=a"
    std::string path;     ///< target without the query string
    std::string query;    ///< raw query string, without '?'
    std::string body;
    /** When the first byte of the request was read. */
    Clock::time_point firstData{};

    /** First value of a query parameter, URL-decoded ('+' is a space);
        nullopt if absent. */
    std::optional<std::string> param(const std::string& name) const;
};

struct HttpResponse {
    int status = 200;
    std::string body;
    std::string contentType = "text/plain";
};

/** Answers one request, once; later calls are ignored. Thread safe, cheap to
    copy, may outlive the server (then send does nothing). */
class HttpResponder {
public:
    void send(HttpResponse response) const;

    struct State;
    explicit HttpResponder(std::shared_ptr<State> state) : state_(std::move(state)) {}

private:
    std::shared_ptr<State> state_;
};

using HttpHandler = std::function<void(const HttpRequest&, HttpResponder)>;

class HttpServer {
public:
    struct Options {
        std::string host = "127.0.0.1";
        int port = 0;                 ///< 0 = pick a free port, see port()
        int threads = 2;              ///< event loop threads
        double idleTimeoutMs = 5000;
        size_t maxBodyBytes = 1 << 20;
        double drainTimeoutMs = 3000; ///< stop() waits this long for answers
        std::string name = "http";    ///< for error messages and logs
    };

    HttpServer();
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    /** Exact path match. Call before start(); a later route replaces one
        with the same method and path. */
    void route(std::string method, std::string path, HttpHandler handler);

    /** Listens, then returns. Throws std::runtime_error if it can not bind.
        A second call while running does nothing. */
    void start(const Options& options);
    /** Safe to call twice; start() works again afterwards. */
    void stop();
    bool running() const { return bool(loop_); }
    /** The bound port while running, else 0. */
    int port() const { return port_; }

    /** Runs fn on an event loop thread. Thread safe; no-op when stopped. */
    void post(std::function<void()> fn) const;
    /** Runs fn on an event loop thread at `when`. Thread safe; no-op when
        stopped (and never runs if the server stops before then). */
    void runAt(HttpRequest::Clock::time_point when, std::function<void()> fn) const;

    struct Loop;      // implementation (event loop, acceptor, sessions)

private:
    struct Routes;
    std::shared_ptr<Routes> routes_;
    std::shared_ptr<Loop> loop_;
    int port_ = 0;
};

} // namespace RTBKIT

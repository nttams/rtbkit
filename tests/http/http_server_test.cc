// HttpServer over real sockets on a free port.

#include "http/http_server.h"

#include <catch2/catch_test_macros.hpp>

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

using namespace RTBKIT;

namespace {

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point begin)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
}

HttpServer::Options options(int threads = 1)
{
    HttpServer::Options o;
    o.port = 0;
    o.threads = threads;
    return o;
}

httplib::Result get(int port, const std::string& path)
{
    httplib::Client client("127.0.0.1", port);
    client.set_read_timeout(5, 0);
    return client.Get(path);
}

} // namespace

TEST_CASE("http server: query parameters are URL-decoded")
{
    HttpRequest r;
    r.query = "a=1&b=x%20y+z&c&d=&e%26=%3D&a=2";
    CHECK(r.param("a") == "1");
    CHECK(r.param("b") == "x y z");
    CHECK(r.param("c") == "");
    CHECK(r.param("d") == "");
    CHECK(r.param("e&") == "=");
    CHECK_FALSE(r.param("missing"));
}

TEST_CASE("http server: routes, 404 and request fields")
{
    HttpServer server;
    std::mutex mutex;
    HttpRequest seen;
    server.route("POST", "/echo", [&](const HttpRequest& r, HttpResponder responder) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            seen = r;
        }
        responder.send({200, r.body, "application/json"});
    });
    server.start(options());

    httplib::Client client("127.0.0.1", server.port());
    auto res = client.Post("/echo?x=1", "{\"a\":1}", "application/json");
    REQUIRE(res);
    CHECK(res->status == 200);
    CHECK(res->body == "{\"a\":1}");
    CHECK(res->get_header_value("Content-Type") == "application/json");
    {
        std::lock_guard<std::mutex> lock(mutex);
        CHECK(seen.method == "POST");
        CHECK(seen.path == "/echo");
        CHECK(seen.query == "x=1");
        CHECK(seen.firstData <= Clock::now());
    }

    auto wrongMethod = client.Get("/echo");
    REQUIRE(wrongMethod);
    CHECK(wrongMethod->status == 404);
    auto unknown = client.Get("/nothing");
    REQUIRE(unknown);
    CHECK(unknown->status == 404);
}

TEST_CASE("http server: a late answer does not hold the event loop thread")
{
    // One thread. A slow request is answered 300 ms later from another
    // thread; meanwhile a second connection is served at once.
    HttpServer server;
    std::mutex mutex;
    std::optional<HttpResponder> slow;
    server.route("GET", "/slow", [&](const HttpRequest&, HttpResponder r) {
        std::lock_guard<std::mutex> lock(mutex);
        slow = std::move(r);
    });
    server.route("GET", "/fast", [](const HttpRequest&, HttpResponder r) { r.send({200, "fast"}); });
    server.start(options(1));

    httplib::Result slowRes;
    auto begin = Clock::now();
    std::thread slowClient([&] { slowRes = get(server.port(), "/slow"); });
    while (true) {
        std::lock_guard<std::mutex> lock(mutex);
        if (slow) break;
    }

    auto fast = get(server.port(), "/fast");
    REQUIRE(fast);
    CHECK(fast->body == "fast");
    CHECK(msSince(begin) < 250);

    std::thread answerer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        std::lock_guard<std::mutex> lock(mutex);
        slow->send({200, "slow"});
        slow->send({500, "ignored"});   // only the first answer counts
    });
    slowClient.join();
    answerer.join();
    REQUIRE(slowRes);
    CHECK(slowRes->status == 200);
    CHECK(slowRes->body == "slow");
    CHECK(msSince(begin) >= 300);
}

TEST_CASE("http server: a responder dropped without an answer is a 500")
{
    HttpServer server;
    server.route("GET", "/forget", [](const HttpRequest&, HttpResponder) {});
    server.route("GET", "/throw", [](const HttpRequest&, HttpResponder) {
        throw std::runtime_error("boom");
    });
    server.start(options());

    auto forget = get(server.port(), "/forget");
    REQUIRE(forget);
    CHECK(forget->status == 500);
    auto thrown = get(server.port(), "/throw");
    REQUIRE(thrown);
    CHECK(thrown->status == 500);
}

TEST_CASE("http server: keep-alive serves several requests on one connection")
{
    HttpServer server;
    std::atomic<int> calls{0};
    server.route("GET", "/n", [&](const HttpRequest&, HttpResponder r) {
        r.send({200, std::to_string(++calls)});
    });
    server.start(options());

    httplib::Client client("127.0.0.1", server.port());
    client.set_keep_alive(true);
    for (int i = 1; i <= 5; ++i) {
        auto res = client.Get("/n");
        REQUIRE(res);
        CHECK(res->body == std::to_string(i));
    }
}

TEST_CASE("http server: post and runAt run on the loop; stop does not wait for far timers")
{
    HttpServer server;
    server.start(options(2));

    std::atomic<int> ran{0};
    server.post([&] { ++ran; });
    server.runAt(Clock::now() + std::chrono::milliseconds(20), [&] { ++ran; });
    server.runAt(Clock::now() + std::chrono::hours(1), [&] { ran += 100; });
    auto begin = Clock::now();
    while (ran < 2 && msSince(begin) < 2000) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(ran == 2);

    begin = Clock::now();
    server.stop();
    CHECK(msSince(begin) < 500);
    CHECK(ran == 2);

    // Stopped: both are no-ops.
    server.post([&] { ran += 1000; });
    server.runAt(Clock::now(), [&] { ran += 1000; });
    CHECK(ran == 2);
}

TEST_CASE("http server: stop waits for an answer in progress, up to the drain timeout")
{
    HttpServer server;
    std::mutex mutex;
    std::vector<HttpResponder> held;
    server.route("GET", "/later", [&](const HttpRequest&, HttpResponder r) {
        std::lock_guard<std::mutex> lock(mutex);
        held.push_back(std::move(r));
    });
    auto o = options();
    o.drainTimeoutMs = 300;
    server.start(o);
    const int port = server.port();

    // Answered during the drain.
    httplib::Result answered;
    std::thread first([&] { answered = get(port, "/later"); });
    while (true) {
        std::lock_guard<std::mutex> lock(mutex);
        if (held.size() == 1) break;
    }
    std::thread answerer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::lock_guard<std::mutex> lock(mutex);
        held[0].send({200, "late"});
    });
    auto begin = Clock::now();
    server.stop();
    first.join();
    answerer.join();
    REQUIRE(answered);
    CHECK(answered->body == "late");
    CHECK(answered->get_header_value("Connection") == "close");
    CHECK(msSince(begin) < 280);

    // Never answered: cut at the drain timeout.
    server.start(o);
    httplib::Result cut;
    std::thread second([&, p = server.port()] { cut = get(p, "/later"); });
    while (true) {
        std::lock_guard<std::mutex> lock(mutex);
        if (held.size() == 2) break;
    }
    begin = Clock::now();
    server.stop();
    second.join();
    CHECK_FALSE(cut);
    CHECK(msSince(begin) >= 250);
    CHECK(msSince(begin) < 2000);

    // A responder that outlives the server does nothing.
    held[1].send({200, "too late"});
    CHECK_FALSE(server.running());
}

TEST_CASE("http server: a taken port is an error")
{
    HttpServer a, b;
    a.start(options());
    auto o = options();
    o.port = a.port();
    CHECK_THROWS_AS(b.start(o), std::runtime_error);
    CHECK_FALSE(b.running());
}

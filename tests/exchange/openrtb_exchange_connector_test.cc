// OpenRtbExchangeConnector over real HTTP on a free port.

#include "exchange/openrtb_exchange_connector.h"

#include "fixtures.h"

#include <catch2/catch_test_macros.hpp>

#include <httplib.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

using namespace RTBKIT;
using namespace RTBKIT::Test;
using nlohmann::json;

namespace {

using Clock = std::chrono::steady_clock;

/** Plays the router: runs jobs on its own thread, like the real router loop. */
class FakeRouter {
public:
    FakeRouter() : thread_([this] { run(); }) {}
    ~FakeRouter()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }

    void post(std::function<void()> job)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            jobs_.push_back(std::move(job));
        }
        cv_.notify_all();
    }

private:
    void run()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while (true) {
            cv_.wait(lock, [&] { return stop_ || !jobs_.empty(); });
            if (jobs_.empty()) return;
            auto job = std::move(jobs_.front());
            jobs_.pop_front();
            lock.unlock();
            job();
            lock.lock();
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> jobs_;
    bool stop_ = false;
    std::thread thread_;
};

std::shared_ptr<AgentConfig> bidderConfig(const std::string& providerConfig = "{}")
{
    auto config = std::make_shared<AgentConfig>(AgentConfig::parse(
        R"({"account": "campaign:strategy",
            "creatives": [{"id": 7, "format": "300x250", "providerConfig": )"
        + providerConfig + "}]}"));
    config->name = "agent1";
    return config;
}

Response bid(const std::shared_ptr<AgentConfig>& config, int spot, double cpm)
{
    Response r;
    r.agent = config->name;
    r.account = config->account;
    r.price = USD_CPM(cpm);
    r.spotIndex = spot;
    r.creativeIndex = 0;
    r.config = config;
    return r;
}

struct Fixture {
    explicit Fixture(json config = json::object())
    {
        config["port"] = 0;
        config["winNoticeUrl"] = "http://127.0.0.1:12360/win";
        connector.configure(config);
    }
    ~Fixture() { connector.stop(); }

    void start()
    {
        connector.start();
        REQUIRE(connector.port() > 0);
    }

    httplib::Result post(const std::string& body, const std::string& path = "/auctions")
    {
        httplib::Client client("127.0.0.1", connector.port());
        client.set_read_timeout(5, 0);
        return client.Post(path, body, "application/json");
    }

    OpenRtbExchangeConnector connector;
};

std::string requestBody(double tmax = 100, std::string id = "auction-1")
{
    json req = json::parse(readTestFile("openrtb_banner.json"));
    req["id"] = id;
    req["tmax"] = tmax;
    return req.dump();
}

} // namespace

TEST_CASE("openrtb exchange connector: bid response carries price, impid, creative and nurl")
{
    Fixture f;
    auto config = bidderConfig(R"({"openrtb": {"adm": "<div>ad</div>", "crid": "cr-7",
                                               "adomain": ["adv.example.com"]}})");
    REQUIRE(f.connector.prepareConfig(*config));

    FakeRouter router;
    f.connector.setAuctionHandler([&](std::shared_ptr<Auction> auction) {
        router.post([auction, config] {
            auction->setResponse(bid(config, 0, 2.5));
            auction->finish();
        });
    });
    f.start();

    auto res = f.post(requestBody(100, "a b&c"));
    REQUIRE(res);
    CHECK(res->status == 200);
    CHECK(res->get_header_value("Content-Type") == "application/json");

    json body = json::parse(res->body);
    CHECK(body["id"] == "a b&c");
    CHECK(body["cur"] == "USD");
    REQUIRE(body["seatbid"].size() == 1);
    REQUIRE(body["seatbid"][0]["bid"].size() == 1);
    const json& b = body["seatbid"][0]["bid"][0];
    CHECK(b["impid"] == "1");
    CHECK(b["price"].get<double>() == 2.5);
    CHECK(b["crid"] == "cr-7");
    CHECK(b["adm"] == "<div>ad</div>");
    CHECK(b["adomain"] == json::array({"adv.example.com"}));
    CHECK(b["cid"] == "agent1");
    CHECK(b["w"] == 300);
    CHECK(b["h"] == 250);
    CHECK(b["nurl"] == "http://127.0.0.1:12360/win?auctionId=a%20b%26c&adSpotId=1"
                       "&price=${AUCTION_PRICE}");
    CHECK(b["id"].is_string());

    auto stats = f.connector.stats();
    CHECK(stats.requests == 1);
    CHECK(stats.bids == 1);
    CHECK(stats.noBids == 0);
}

TEST_CASE("openrtb exchange connector: creative id is the default crid and providerConfig is optional")
{
    Fixture f;
    auto config = bidderConfig();   // not prepared: falls back to the raw providerConfig
    f.connector.setAuctionHandler([&](std::shared_ptr<Auction> auction) {
        auction->setResponse(bid(config, 0, 1.25));
        auction->finish();
    });
    f.start();

    auto res = f.post(requestBody());
    REQUIRE(res);
    REQUIRE(res->status == 200);
    const json b = json::parse(res->body)["seatbid"][0]["bid"][0];
    CHECK(b["crid"] == "7");
    CHECK_FALSE(b.contains("adm"));
    CHECK(b["price"].get<double>() == 1.25);
}

TEST_CASE("openrtb exchange connector: only spots with a bid are answered")
{
    Fixture f;
    auto config = bidderConfig();
    f.connector.setAuctionHandler([&](std::shared_ptr<Auction> auction) {
        auction->setResponse(bid(config, 1, 0.75));
        auction->finish();
    });
    f.start();

    json req = json::parse(requestBody());
    req["imp"].push_back({{"id", "2"}, {"banner", {{"w", 300}, {"h", 250}}}});
    auto res = f.post(req.dump());
    REQUIRE(res);
    REQUIRE(res->status == 200);
    json bids = json::parse(res->body)["seatbid"][0]["bid"];
    REQUIRE(bids.size() == 1);
    CHECK(bids[0]["impid"] == "2");
    CHECK(bids[0]["price"].get<double>() == 0.75);
}

TEST_CASE("openrtb exchange connector: no response is a 204")
{
    Fixture f;
    f.connector.setAuctionHandler([](std::shared_ptr<Auction> auction) { auction->finish(); });
    f.start();

    auto res = f.post(requestBody());
    REQUIRE(res);
    CHECK(res->status == 204);
    CHECK(res->body.empty());

    auto stats = f.connector.stats();
    CHECK(stats.requests == 1);
    CHECK(stats.noBids == 1);
    CHECK(stats.bids == 0);
}

TEST_CASE("openrtb exchange connector: malformed requests are a 400")
{
    Fixture f;
    f.connector.setAuctionHandler([](std::shared_ptr<Auction> auction) { auction->finish(); });
    f.start();

    for (const char* body : {"not json", "", "{}", R"({"id": "x"})",
                             R"({"id": "x", "imp": []})", "[1, 2]"}) {
        INFO("body: " << body);
        auto res = f.post(body);
        REQUIRE(res);
        CHECK(res->status == 400);
        CHECK(json::parse(res->body).contains("error"));
    }

    auto stats = f.connector.stats();
    CHECK(stats.badRequests == 6);
    CHECK(stats.bids == 0);
}

TEST_CASE("openrtb exchange connector: a router that finishes at the expiry answers in time")
{
    Fixture f;
    FakeRouter router;
    f.connector.setAuctionHandler([&](std::shared_ptr<Auction> auction) {
        router.post([auction] {
            std::this_thread::sleep_until(auction->expiry());
            auction->finish();
        });
    });
    f.start();

    auto begin = Clock::now();
    auto res = f.post(requestBody(80));
    double ms = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
    REQUIRE(res);
    CHECK(res->status == 204);
    CHECK(ms >= 70);
    CHECK(ms < 500);
    CHECK(f.connector.stats().timeouts == 0);
}

TEST_CASE("openrtb exchange connector: a router that never finishes is cut after expiry plus grace")
{
    Fixture f({{"graceMs", 20}});
    std::shared_ptr<Auction> seen;
    std::mutex mutex;
    f.connector.setAuctionHandler([&](std::shared_ptr<Auction> auction) {
        std::lock_guard<std::mutex> lock(mutex);
        seen = std::move(auction);
    });
    f.start();

    auto begin = Clock::now();
    auto res = f.post(requestBody(50));
    double ms = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
    REQUIRE(res);
    CHECK(res->status == 204);
    CHECK(ms >= 60);
    CHECK(ms < 1000);

    std::lock_guard<std::mutex> lock(mutex);
    REQUIRE(seen);
    CHECK(seen->finished());
    auto config = bidderConfig();
    CHECK(seen->setResponse(bid(config, 0, 1.0)) == Auction::SetResult::Finished);
    CHECK(f.connector.stats().timeouts == 1);
    CHECK(f.connector.stats().noBids == 1);
}

TEST_CASE("openrtb exchange connector: tmax is capped by maxTmaxMs")
{
    Fixture f({{"graceMs", 10}, {"maxTmaxMs", 60}});
    f.connector.setAuctionHandler([](std::shared_ptr<Auction>) {});
    f.start();

    auto begin = Clock::now();
    auto res = f.post(requestBody(60000));
    double ms = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
    REQUIRE(res);
    CHECK(res->status == 204);
    CHECK(ms < 2000);
}

TEST_CASE("openrtb exchange connector: a missing auction handler is a 500")
{
    Fixture f;
    f.start();

    auto res = f.post(requestBody());
    REQUIRE(res);
    CHECK(res->status == 500);
    CHECK(f.connector.stats().errors == 1);
}

TEST_CASE("openrtb exchange connector: ping, unknown paths and stop")
{
    Fixture f;
    f.connector.setAuctionHandler([](std::shared_ptr<Auction> auction) { auction->finish(); });
    CHECK(f.connector.port() == 0);
    f.start();
    int port = f.connector.port();

    httplib::Client client("127.0.0.1", port);
    auto ping = client.Get("/ping");
    REQUIRE(ping);
    CHECK(ping->status == 200);
    CHECK(ping->body == "pong");

    auto other = client.Post("/nothing", "{}", "application/json");
    REQUIRE(other);
    CHECK(other->status == 404);

    f.connector.start();   // second start is a no-op
    CHECK(f.connector.port() == port);

    f.connector.stop();
    f.connector.stop();
    CHECK(f.connector.port() == 0);
    httplib::Client closed("127.0.0.1", port);
    CHECK_FALSE(closed.Get("/ping"));

    // It can be started again.
    f.start();
    httplib::Client again("127.0.0.1", f.connector.port());
    auto res = again.Get("/ping");
    REQUIRE(res);
    CHECK(res->status == 200);
}

TEST_CASE("openrtb exchange connector: many concurrent requests")
{
    Fixture f({{"threads", 16}});
    auto config = bidderConfig();
    FakeRouter router;
    f.connector.setAuctionHandler([&](std::shared_ptr<Auction> auction) {
        router.post([auction, config] {
            // Even spots bid, so half of the auctions are no-bids.
            if (std::hash<std::string>()(auction->id().toString()) % 2 == 0)
                auction->setResponse(bid(config, 0, 1.5));
            auction->finish();
        });
    });
    f.start();

    constexpr int clients = 8, perClient = 25;
    std::vector<std::thread> threads;
    std::atomic<int> ok{0}, failed{0};
    for (int c = 0; c < clients; ++c) {
        threads.emplace_back([&, c] {
            httplib::Client client("127.0.0.1", f.connector.port());
            client.set_read_timeout(5, 0);
            for (int i = 0; i < perClient; ++i) {
                std::string id = "c" + std::to_string(c) + "-" + std::to_string(i);
                auto res = client.Post("/auctions", requestBody(200, id), "application/json");
                bool good = res && (res->status == 204
                    || (res->status == 200 && json::parse(res->body)["id"] == id));
                ++(good ? ok : failed);
            }
        });
    }
    for (auto& t : threads) t.join();

    CHECK(ok == clients * perClient);
    CHECK(failed == 0);
    auto stats = f.connector.stats();
    CHECK(stats.requests == uint64_t(clients * perClient));
    CHECK(stats.bids + stats.noBids == stats.requests);
}

TEST_CASE("openrtb exchange connector: configure")
{
    OpenRtbExchangeConnector connector;
    CHECK(connector.exchangeName() == "openrtb");
    CHECK(connector.winNoticeUrl() == "http://127.0.0.1:12360/win");

    connector.configure({{"name", "mock"}, {"port", 0}, {"threads", 4},
                         {"winNoticeUrl", "http://ads.example.com/win"}});
    CHECK(connector.exchangeName() == "mock");
    CHECK(connector.winNoticeUrl() == "http://ads.example.com/win");

    CHECK_THROWS_AS(connector.configure({{"port", "x"}}), std::runtime_error);
    CHECK_THROWS_AS(connector.configure({{"port", 70000}}), std::runtime_error);
    CHECK_THROWS_AS(connector.configure({{"threads", 0}}), std::runtime_error);
    CHECK_THROWS_AS(connector.configure({{"winNoticeUrl", ""}}), std::runtime_error);
    CHECK_THROWS_AS(connector.configure({{"name", ""}}), std::runtime_error);
    CHECK_THROWS_AS(connector.configure({{"maxTmaxMs", 0}}), std::runtime_error);
    CHECK_THROWS_AS(connector.configure(json::array()), std::runtime_error);
    CHECK(connector.exchangeName() == "mock");   // failed configure changes nothing
}

TEST_CASE("openrtb exchange connector: provider data follows providerConfig.<exchange>")
{
    OpenRtbExchangeConnector connector;
    connector.configure({{"name", "mock"}});

    // Everything is accepted by default.
    AgentConfig plain = sampleAgentConfig();
    REQUIRE(connector.prepareConfig(plain));
    CHECK(plain.providerData.at("mock") != nullptr);
    CHECK(plain.creatives[0].providerData.at("mock") != nullptr);

    // A config for another exchange does not matter.
    auto other = bidderConfig(R"({"openrtb": {"adm": 5}})");
    REQUIRE(connector.prepareConfig(*other));
    CHECK(other->creatives[0].providerData.count("mock") == 1);

    // Good settings for this exchange are accepted, bad types are not.
    auto good = bidderConfig(R"({"mock": {"adm": "x", "crid": 12, "adomain": ["a.com"]}})");
    REQUIRE(connector.prepareConfig(*good));
    CHECK(good->creatives[0].providerData.count("mock") == 1);

    for (const char* bad : {R"({"mock": {"adm": 5}})", R"({"mock": {"crid": [1]}})",
                            R"({"mock": {"adomain": "a.com"}})",
                            R"({"mock": {"adomain": [1]}})", R"({"mock": "x"})"}) {
        INFO(bad);
        auto config = bidderConfig(bad);
        REQUIRE(connector.prepareConfig(*config));
        CHECK(config->creatives[0].providerData.count("mock") == 0);
    }
}

TEST_CASE("openrtb exchange connector: buildBidResponse of an auction without bids is empty")
{
    OpenRtbExchangeConnector connector;
    auto auction = Auction::create(sampleBidRequest());
    CHECK(connector.buildBidResponse(*auction).empty());

    auto config = bidderConfig();
    auction->setResponse(bid(config, 0, 3.0));
    json response = json::parse(connector.buildBidResponse(*auction));
    CHECK(response["id"] == "auction-1");
    CHECK(response["seatbid"][0]["bid"][0]["impid"] == "1");
    CHECK(response["seatbid"][0]["bid"][0]["price"].get<double>() == 3.0);
}

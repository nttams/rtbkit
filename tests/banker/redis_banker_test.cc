// Runs only with RTB_WITH_REDIS. Every test starts its own redis-server on a
// free port (see redis_test_server.h) and SKIPs if there is none.
#ifdef RTB_WITH_REDIS

#include "banker/banker_factory.h"
#include "banker/redis_banker.h"

#include "banker_contract.h"
#include "redis_test_server.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <functional>
#include <thread>

using namespace RTBKIT;
using banker_test::RedisServer;
using Clock = std::chrono::steady_clock;

namespace {

// The background thread only runs every 100 s here, so tests drive the sync
// rounds themselves with sync() and are deterministic. The block (target
// allocation) is spendRate * 100 s; `block` must be a multiple of 100.
RedisBankerConfig manualConfig(const RedisServer& server, int64_t block = 100000)
{
    RedisBankerConfig config;
    config.port = server.port();
    config.prefix = "t";
    config.syncIntervalMs = 100000;
    config.spendRate = block / 100;
    config.ioTimeoutMs = 300;
    return config;
}

std::string redisAcct(const std::string& name) { return "t:acct:" + name; }

bool waitFor(const std::function<bool()>& condition, int seconds = 10)
{
    auto deadline = Clock::now() + std::chrono::seconds(seconds);
    while (!condition() && Clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return condition();
}

} // namespace

#define START_REDIS(server)                                                    \
    auto server = RedisServer::start();                                        \
    if (!server) SKIP("no redis-server available");

TEST_CASE("redis banker: budgets and hierarchy")
{
    START_REDIS(server);
    RedisBanker banker(manualConfig(*server));
    banker_test::checkBudgetAndHierarchy(banker);
}

TEST_CASE("redis banker: settlement")
{
    START_REDIS(server);
    RedisBanker banker(manualConfig(*server));
    banker_test::checkSettlement(banker);
}

TEST_CASE("redis banker: ancestor budget caps allocation when it is handed out")
{
    START_REDIS(server);
    RedisBanker banker(manualConfig(*server));
    // the parent has children, so it holds no allocation itself
    banker.createAccount("camp:a");
    banker.createAccount("camp:b");
    banker.setBudget("camp", MicroUSD(110));
    banker.setBudget("camp:a", MicroUSD(100));
    banker.setBudget("camp:b", MicroUSD(50));
    CHECK(server->field(redisAcct("camp"), "allocated") == 110);   // from the two children

    CHECK(banker.authorizeBid("camp:a", "a1", MicroUSD(100)));
    CHECK_FALSE(banker.authorizeBid("camp:b", "b1", MicroUSD(11)));   // only 10 left in camp
    CHECK(banker.authorizeBid("camp:b", "b1", MicroUSD(10)));
    CHECK_FALSE(banker.authorizeBid("camp:b", "b2", MicroUSD(1)));
    CHECK(banker.getAccountSummary("camp").budget == MicroUSD(110));
    CHECK(banker.getAccountSummary("camp").inFlight == MicroUSD(110));

    // more room in the parent: the round of setBudget gives camp:b the rest of its budget
    banker.setBudget("camp", MicroUSD(1000));
    banker.sync();
    CHECK_FALSE(banker.authorizeBid("camp:b", "b3", MicroUSD(41)));   // leaf budget 50 - 10
    CHECK(banker.authorizeBid("camp:b", "b3", MicroUSD(40)));

    // allocation already held is not taken back when an ancestor budget drops
    banker.setBudget("camp", MicroUSD(1));
    banker.cancelBid("camp:b", "b3");
    CHECK(banker.authorizeBid("camp:b", "b4", MicroUSD(40)));
}

TEST_CASE("redis banker: spend is reported and allocation topped up")
{
    START_REDIS(server);
    RedisBanker banker(manualConfig(*server, 100));   // block of 100
    banker.setBudget("c:s", MicroUSD(1000));
    CHECK(banker.heldAllocation("c:s") == MicroUSD(100));
    CHECK(server->field(redisAcct("c:s"), "allocated") == 100);

    CHECK(banker.authorizeBid("c:s", "1", MicroUSD(100)));
    CHECK_FALSE(banker.authorizeBid("c:s", "2", MicroUSD(1)));   // the block is used up
    banker.winBid("c:s", "1", MicroUSD(60));                     // price below the bid
    CHECK(server->field(redisAcct("c:s"), "spent") == 0);             // not reported yet

    REQUIRE(banker.sync());
    CHECK(server->field(redisAcct("c:s"), "spent") == 60);
    CHECK(server->field(redisAcct("c"), "spent") == 60);              // ancestors too
    // the target is twice the demand of the round (200): 40 left + 160 asked
    CHECK(server->field(redisAcct("c:s"), "allocated") == 200);
    CHECK(banker.authorizeBid("c:s", "2", MicroUSD(100)));
    CHECK(banker.getAccountSummary("c:s").spent == MicroUSD(60));
    CHECK(banker.getAccountSummary("c:s").inFlight == MicroUSD(100));
}

TEST_CASE("redis banker: allocation never goes past the budget")
{
    START_REDIS(server);
    RedisBanker banker(manualConfig(*server, 100));
    banker.setBudget("c:s", MicroUSD(150));
    int won = 0;
    for (int i = 0; i < 10; ++i) {
        if (banker.authorizeBid("c:s", std::to_string(i), MicroUSD(25))) {
            banker.winBid("c:s", std::to_string(i), MicroUSD(25));
            ++won;
        }
        if (i % 3 == 2) banker.sync();
    }
    banker.sync();
    CHECK(won == 6);
    CHECK(server->field(redisAcct("c:s"), "spent") == 150);
    CHECK(banker.getAccountSummary("c:s").available().isZero());
}

TEST_CASE("redis banker: two bankers share one budget")
{
    START_REDIS(server);
    RedisBanker a(manualConfig(*server, 100));
    RedisBanker b(manualConfig(*server, 100));
    a.setBudget("x:y", MicroUSD(150));

    // b does not know the account: refused, found by its next round
    CHECK_FALSE(b.authorizeBid("x:y", "b1", MicroUSD(1)));
    REQUIRE(b.sync());
    CHECK(a.heldAllocation("x:y") == MicroUSD(100));
    CHECK(b.heldAllocation("x:y") == MicroUSD(50));          // what was left
    CHECK(b.accounts() == std::vector<AccountKey>{"x", "x:y"});
    CHECK(b.getAccountSummary("x:y").budget == MicroUSD(150));

    CHECK(b.authorizeBid("x:y", "b1", MicroUSD(50)));
    CHECK_FALSE(b.authorizeBid("x:y", "b2", MicroUSD(1)));

    // a gives everything back on shutdown, b can get it in its next round
    a.shutdown();
    CHECK_FALSE(a.authorizeBid("x:y", "a1", MicroUSD(1)));
    CHECK(server->field(redisAcct("x:y"), "allocated") == 50);
    b.cancelBid("x:y", "b1");
    b.winBid("x:y", "b1", MicroUSD(20));
    REQUIRE(b.sync());
    CHECK(server->field(redisAcct("x:y"), "spent") == 20);
    CHECK(b.getAccountSummary("x").spent == MicroUSD(20));
    CHECK(b.authorizeBid("x:y", "b3", MicroUSD(100)));       // 150 - 20 spent is free again
}

TEST_CASE("redis banker: concurrent bankers never spend more than the budget")
{
    START_REDIS(server);
    {
        RedisBanker setup(manualConfig(*server, 100));
        setup.setBudget("c:s", MicroUSD(500));
    }
    std::atomic<int> won{0};
    std::vector<std::thread> pool;
    for (int t = 0; t < 4; ++t)
        pool.emplace_back([&, t] {
            RedisBankerConfig config = manualConfig(*server, 100);
            config.syncIntervalMs = 5;      // real background rounds
            config.spendRate = 8000;        // block 40
            RedisBanker banker(config);
            banker.createAccount("c:s");
            for (int i = 0; i < 400; ++i) {
                std::string item = std::to_string(t) + "-" + std::to_string(i);
                if (banker.authorizeBid("c:s", item, MicroUSD(2))) {
                    banker.winBid("c:s", item, MicroUSD(1));
                    ++won;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
        });                                  // the destructor flushes and releases
    for (auto& th : pool) th.join();

    long long spent = server->field(redisAcct("c:s"), "spent");
    CHECK(spent == won);
    CHECK(spent <= 500);
    CHECK(spent > 0);
    CHECK(server->field(redisAcct("c:s"), "allocated") == 0);
    CHECK(server->field(redisAcct("c"), "allocated") == 0);
}

TEST_CASE("redis banker: authorizeBid does not wait for Redis")
{
    START_REDIS(server);
    RedisBankerConfig config = manualConfig(*server);
    config.syncIntervalMs = 50;
    config.spendRate = 20'000'000;    // block 1'000'000
    config.ioTimeoutMs = 1000;
    RedisBanker banker(config);
    banker.setBudget("p:q", MicroUSD(100'000'000));
    REQUIRE(banker.heldAllocation("p:q") == MicroUSD(1'000'000));

    server->pause();                                        // accepts, never answers
    std::this_thread::sleep_for(std::chrono::milliseconds(200));   // thread is stuck in I/O

    auto start = Clock::now();
    Clock::duration slowest{};
    for (int i = 0; i < 5000; ++i) {
        auto t0 = Clock::now();
        std::string item = std::to_string(i);
        REQUIRE(banker.authorizeBid("p:q", item, MicroUSD(10)));
        if (i % 2) banker.winBid("p:q", item, MicroUSD(5));
        else banker.cancelBid("p:q", item);
        slowest = std::max(slowest, Clock::now() - t0);
    }
    banker.forceWinBid("p:q", "late", MicroUSD(1));
    auto total = Clock::now() - start;
    // one blocking round trip would cost the whole 1 s timeout
    CHECK(total < std::chrono::milliseconds(300));
    CHECK(slowest < std::chrono::milliseconds(100));
    CHECK(banker.getAccountSummary("p:q").inFlight.isZero());

    // an amount over the allocation is refused at once, no exception
    start = Clock::now();
    CHECK_FALSE(banker.authorizeBid("p:q", "big", MicroUSD(2'000'000)));
    CHECK(Clock::now() - start < std::chrono::milliseconds(100));

    // Two rounds time out while paused; the second one carried the spend and
    // reached the server's socket. After the resume it is sent again with the
    // same sequence number, so the spend is counted exactly once.
    REQUIRE(waitFor([&] { return banker.failedSyncs() >= 2; }));
    server->resume();
    // 2500 wins of 5 and the forced win of 1
    CHECK(waitFor([&] { return server->field(redisAcct("p:q"), "spent") == 12501; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(server->field(redisAcct("p:q"), "spent") == 12501);
}

TEST_CASE("redis banker: unreachable Redis")
{
    START_REDIS(server);
    RedisBankerConfig config = manualConfig(*server, 100);
    config.syncIntervalMs = 20;
    config.spendRate = 5000;          // block 100
    config.ioTimeoutMs = 100;
    RedisBanker banker(config);
    banker.setBudget("d:e", MicroUSD(1000));
    server->halt();                                         // connection refused from now on

    CHECK(banker.authorizeBid("d:e", "1", MicroUSD(100)));   // the allocation is still there
    CHECK_FALSE(banker.authorizeBid("d:e", "2", MicroUSD(1)));
    banker.winBid("d:e", "1", MicroUSD(100));
    CHECK(waitFor([&] { return banker.failedSyncs() > 0; }));
    CHECK_FALSE(banker.authorizeBid("d:e", "3", MicroUSD(1)));   // no top-up without Redis
    CHECK_FALSE(banker.sync());
    CHECK(banker.getAccountSummary("d:e").spent == MicroUSD(100));   // local view
    CHECK_THROWS_AS(banker.setBudget("d:e", MicroUSD(5)), std::runtime_error);
    CHECK_THROWS_AS(banker.createAccount("d:f"), std::runtime_error);

    // a banker that never saw Redis starts fine and refuses bids
    RedisBanker lonely(config);
    CHECK_FALSE(lonely.authorizeBid("d:e", "1", MicroUSD(1)));
    lonely.cancelBid("d:e", "1");
    lonely.winBid("d:e", "9", MicroUSD(3));
    CHECK(lonely.getAccountSummary("d:e").spent == MicroUSD(3));
}   // both destructors return without hanging

TEST_CASE("redis banker: shutdown flushes spend and releases allocation")
{
    START_REDIS(server);
    {
        RedisBanker banker(manualConfig(*server, 100));
        banker.setBudget("s:t", MicroUSD(1000));
        CHECK(banker.authorizeBid("s:t", "w", MicroUSD(40)));
        CHECK(banker.authorizeBid("s:t", "open", MicroUSD(30)));   // never settled
        banker.winBid("s:t", "w", MicroUSD(35));
        banker.forceWinBid("s:t", "f", MicroUSD(200));             // beyond the allocation
        CHECK(server->field(redisAcct("s:t"), "allocated") == 100);
        CHECK(server->field(redisAcct("s:t"), "spent") == 0);
    }
    CHECK(server->field(redisAcct("s:t"), "spent") == 235);
    CHECK(server->field(redisAcct("s:t"), "allocated") == 0);
    CHECK(server->field(redisAcct("s"), "spent") == 235);
    CHECK(server->field(redisAcct("s"), "allocated") == 0);
}

TEST_CASE("redis banker: factory")
{
    START_REDIS(server);
    nlohmann::json config = {{"type", "redis"}, {"host", "127.0.0.1"},
                             {"port", server->port()}, {"prefix", "f"},
                             {"syncIntervalMs", 50}, {"spendRate", "2000USD/1M"},
                             {"accounts", {{"a", {{"budget", "1000USD"}}},
                                           {"a:b", {{"budget", "100USD"}}}}}};
    auto banker = makeBanker(config);
    auto* redis = dynamic_cast<RedisBanker*>(banker.get());
    REQUIRE(redis != nullptr);
    CHECK(banker->getAccountSummary("a").budget == MicroUSD(1000));
    CHECK(redis->heldAllocation("a:b") == MicroUSD(100));
    CHECK(redis->heldAllocation("a").isZero());              // children first: no hoarding
    CHECK(banker->authorizeBid("a:b", "i", MicroUSD(100)));

    // Redis down: no accounts means no error, with accounts there is one
    server->halt();
    nlohmann::json down = {{"type", "redis"}, {"port", server->port()}, {"ioTimeoutMs", 100}};
    CHECK_NOTHROW(makeBanker(down));
    down["accounts"] = {{"a:b", {{"budget", "100USD"}}}};
    CHECK_THROWS_AS(makeBanker(down), std::runtime_error);
    down.erase("accounts");
    down["syncIntervalMs"] = 0;
    CHECK_THROWS_AS(makeBanker(down), std::runtime_error);
    down["syncIntervalMs"] = 10;
    down["spendRate"] = "-1USD";
    CHECK_THROWS_AS(makeBanker(down), std::runtime_error);
}

#endif // RTB_WITH_REDIS

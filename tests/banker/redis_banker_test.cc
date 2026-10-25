// Runs only when built with RTB_WITH_REDIS and a Redis server answers on
// RTB_REDIS_HOST:RTB_REDIS_PORT (default 127.0.0.1:6379); otherwise SKIP.
#ifdef RTB_WITH_REDIS

#include "banker/banker_factory.h"
#include "banker/redis_banker.h"

#include "banker_contract.h"

#include <hiredis/hiredis.h>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdlib>
#include <thread>

using namespace RTBKIT;

namespace {

RedisBankerConfig testConfig()
{
    RedisBankerConfig config;
    if (const char* host = std::getenv("RTB_REDIS_HOST")) config.host = host;
    if (const char* port = std::getenv("RTB_REDIS_PORT")) config.port = std::atoi(port);
    config.prefix = "rtbtest" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    return config;
}

// Removes every key of the test prefix when it goes out of scope.
struct Cleanup {
    RedisBankerConfig config;
    ~Cleanup()
    {
        redisContext* c = redisConnect(config.host.c_str(), config.port);
        if (!c) return;
        if (!c->err) {
            std::string pattern = config.prefix + ":*";
            void* keys = redisCommand(c, "KEYS %s", pattern.c_str());
            auto* reply = static_cast<redisReply*>(keys);
            if (reply && reply->type == REDIS_REPLY_ARRAY)
                for (size_t i = 0; i < reply->elements; ++i)
                    freeReplyObject(redisCommand(c, "DEL %s", reply->element[i]->str));
            if (reply) freeReplyObject(reply);
        }
        redisFree(c);
    }
};

} // namespace

#define MAKE_REDIS_BANKER(name)                                                \
    Cleanup cleanup{testConfig()};                                             \
    std::unique_ptr<RedisBanker> name;                                         \
    try { name = std::make_unique<RedisBanker>(cleanup.config); }              \
    catch (const std::runtime_error& e) { SKIP("no redis server: " << e.what()); }

TEST_CASE("redis banker: budgets and hierarchy")
{
    MAKE_REDIS_BANKER(banker);
    banker_test::checkBudgetAndHierarchy(*banker);
}

TEST_CASE("redis banker: settlement")
{
    MAKE_REDIS_BANKER(banker);
    banker_test::checkSettlement(*banker);
}

TEST_CASE("redis banker: two connections share state")
{
    MAKE_REDIS_BANKER(a);
    RedisBanker b(cleanup.config);
    a->setBudget("x:y", MicroUSD(10));
    CHECK(b.authorizeBid("x:y", "1", MicroUSD(10)));
    CHECK_FALSE(a->authorizeBid("x:y", "2", MicroUSD(1)));
}

TEST_CASE("redis banker: concurrent authorize never exceeds the budget")
{
    MAKE_REDIS_BANKER(setup);
    setup->setBudget("c:s", MicroUSD(500));
    std::atomic<int> granted{0};
    std::vector<std::thread> pool;
    for (int t = 0; t < 4; ++t)
        pool.emplace_back([&, t] {
            RedisBanker banker(cleanup.config);   // own connection
            for (int i = 0; i < 300; ++i)
                if (banker.authorizeBid("c:s", std::to_string(t) + "-" + std::to_string(i),
                                        MicroUSD(1)))
                    ++granted;
        });
    for (auto& th : pool) th.join();
    CHECK(granted == 500);
    CHECK(setup->getAccountSummary("c").inFlight == MicroUSD(500));
}

TEST_CASE("redis banker: factory")
{
    MAKE_REDIS_BANKER(probe);
    nlohmann::json config = {{"type", "redis"}, {"host", cleanup.config.host},
                             {"port", cleanup.config.port},
                             {"prefix", cleanup.config.prefix},
                             {"accounts", {{"a:b", {{"budget", "100USD"}}}}}};
    auto banker = makeBanker(config);
    CHECK(dynamic_cast<RedisBanker*>(banker.get()) != nullptr);
    CHECK(banker->getAccountSummary("a").budget == MicroUSD(100));
}

#endif // RTB_WITH_REDIS

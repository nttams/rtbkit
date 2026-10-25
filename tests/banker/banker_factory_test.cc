#include "banker/banker_factory.h"

#include "banker/in_memory_banker.h"

#include <catch2/catch_test_macros.hpp>

using namespace RTBKIT;
using nlohmann::json;

TEST_CASE("makeBanker: memory with accounts")
{
    json config = json::parse(R"({
        "type": "memory",
        "accounts": {
            "camp:s1": {"budget": "100USD"},
            "camp:s2": {"budget": [250, "USD/1M"]},
            "camp:s3": {"budget": 40},
            "other": {}
        }})");
    auto banker = makeBanker(config);
    REQUIRE(dynamic_cast<InMemoryBanker*>(banker.get()) != nullptr);

    CHECK(banker->getAccountSummary("camp:s1").budget == MicroUSD(100));
    CHECK(banker->getAccountSummary("camp:s2").budget == MicroUSD(250));
    CHECK(banker->getAccountSummary("camp:s3").budget == MicroUSD(40));
    CHECK(banker->getAccountSummary("camp").budget == MicroUSD(390));
    CHECK(banker->accounts().size() == 5);   // camp, 3 strategies, other

    CHECK(banker->authorizeBid("camp:s1", "i", MicroUSD(100)));
    CHECK_FALSE(banker->authorizeBid("camp:s1", "j", MicroUSD(1)));
    CHECK_FALSE(banker->authorizeBid("other", "k", MicroUSD(1)));   // no budget
}

TEST_CASE("makeBanker: defaults and null")
{
    CHECK(dynamic_cast<InMemoryBanker*>(makeBanker(json::object()).get()) != nullptr);
    CHECK(dynamic_cast<InMemoryBanker*>(makeBanker(json(nullptr)).get()) != nullptr);

    auto banker = makeBanker(json{{"type", "null"},
                                  {"accounts", {{"a", {{"budget", "1USD"}}}}}});
    CHECK(dynamic_cast<NullBanker*>(banker.get()) != nullptr);
    CHECK(banker->authorizeBid("a", "i", MicroUSD(1'000'000'000)));
}

TEST_CASE("makeBanker: bad config")
{
    CHECK_THROWS_AS(makeBanker(json{{"type", "mongo"}}), std::runtime_error);
    CHECK_THROWS_AS(makeBanker(json{{"type", 3}}), std::runtime_error);
    CHECK_THROWS_AS(makeBanker(json::array()), std::runtime_error);
    CHECK_THROWS_AS(makeBanker(json{{"accounts", 1}}), std::runtime_error);
    CHECK_THROWS_AS(makeBanker(json{{"accounts", {{"a b", {{"budget", "1USD"}}}}}}),
                    std::runtime_error);
    CHECK_THROWS_AS(makeBanker(json{{"accounts", {{"a", {{"budget", "lots"}}}}}}),
                    std::runtime_error);
    CHECK_THROWS_AS(makeBanker(json{{"accounts", {{"a", {{"budget", "-5USD"}}}}}}),
                    std::runtime_error);
    CHECK_THROWS_AS(makeBanker(json{{"accounts", {{"a", {{"budget", -5}}}}}}),
                    std::runtime_error);
}

#ifndef RTB_WITH_REDIS
TEST_CASE("makeBanker: redis needs a redis build")
{
    CHECK_THROWS_AS(makeBanker(json{{"type", "redis"}}), std::runtime_error);
}
#endif

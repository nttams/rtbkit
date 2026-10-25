#include "banker/in_memory_banker.h"

#include "banker_contract.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <thread>

using namespace RTBKIT;

TEST_CASE("in memory banker: budgets and hierarchy")
{
    InMemoryBanker banker;
    banker_test::checkBudgetAndHierarchy(banker);
}

TEST_CASE("in memory banker: settlement")
{
    InMemoryBanker banker;
    banker_test::checkSettlement(banker);
}

TEST_CASE("in memory banker: createAccount and empty budget")
{
    InMemoryBanker banker;
    banker.createAccount("a:b:c");
    CHECK(banker.accounts() == std::vector<AccountKey>{"a", "a:b", "a:b:c"});
    banker.createAccount("a:b:c");   // no-op
    CHECK(banker.accounts().size() == 3);

    // no budget means nothing can be reserved, except zero
    CHECK_FALSE(banker.authorizeBid("a:b:c", "i", MicroUSD(1)));
    CHECK(banker.authorizeBid("a:b:c", "z", Amount()));
}

TEST_CASE("in memory banker: ancestor without budget does not limit")
{
    InMemoryBanker banker;
    banker.setBudget("a:b", MicroUSD(10));
    CHECK(banker.getAccountSummary("a").budget == MicroUSD(10));
    CHECK(banker.authorizeBid("a:b", "i", MicroUSD(10)));
    // the parent itself has no own budget, so it cannot authorize
    CHECK_FALSE(banker.authorizeBid("a", "j", MicroUSD(1)));
}

TEST_CASE("in memory banker: three levels")
{
    InMemoryBanker banker;
    banker.setBudget("a", MicroUSD(100));
    banker.setBudget("a:b", MicroUSD(80));
    banker.setBudget("a:b:c", MicroUSD(80));
    banker.setBudget("a:d", MicroUSD(80));

    CHECK(banker.authorizeBid("a:b:c", "1", MicroUSD(70)));
    CHECK_FALSE(banker.authorizeBid("a:b:c", "2", MicroUSD(11)));   // a:b has 80
    CHECK_FALSE(banker.authorizeBid("a:d", "3", MicroUSD(31)));     // a has 100
    CHECK(banker.authorizeBid("a:d", "3", MicroUSD(30)));

    banker.winBid("a:b:c", "1", MicroUSD(60));
    AccountSummary root = banker.getAccountSummary("a");
    CHECK(root.spent == MicroUSD(60));
    CHECK(root.inFlight == MicroUSD(30));
    CHECK(banker.getAccountSummary("a:b").spent == MicroUSD(60));
    CHECK(banker.getAccountSummary("a:d").spent.isZero());
}

TEST_CASE("in memory banker: summary json")
{
    InMemoryBanker banker;
    banker.setBudget("a:b", MicroUSD(100));
    banker.authorizeBid("a:b", "i", MicroUSD(30));
    nlohmann::json j = banker.summaryJson();
    REQUIRE(j.at("accounts").size() == 2);
    AccountSummary leaf = AccountSummary::fromJson(j.at("accounts").at(1));
    CHECK(leaf.account == AccountKey("a:b"));
    CHECK(leaf.inFlight == MicroUSD(30));
    CHECK(j.at("accounts").at(1).at("available") == nlohmann::json(MicroUSD(70)));
}

TEST_CASE("in memory banker: mixed currencies throw and change nothing")
{
    InMemoryBanker banker;
    banker.setBudget("a", MicroUSD(100));
    CHECK(banker.authorizeBid("a", "usd", MicroUSD(10)));
    CHECK_THROWS_AS(banker.authorizeBid("a", "eur", Amount(CurrencyCode::EUR, 5)),
                    std::invalid_argument);
    CHECK(banker.getAccountSummary("a").inFlight == MicroUSD(10));
}

TEST_CASE("in memory banker: settled item memory is bounded")
{
    InMemoryBanker banker(2);
    banker.setBudget("a", MicroUSD(1000));
    banker.winBid("a", "1", MicroUSD(1));
    banker.winBid("a", "2", MicroUSD(1));
    banker.winBid("a", "3", MicroUSD(1));   // "1" is forgotten
    banker.winBid("a", "3", MicroUSD(1));   // still remembered
    CHECK(banker.getAccountSummary("a").spent == MicroUSD(3));
    banker.winBid("a", "1", MicroUSD(1));   // charged again, as documented
    CHECK(banker.getAccountSummary("a").spent == MicroUSD(4));
}

TEST_CASE("in memory banker: concurrent authorize never exceeds the budget")
{
    InMemoryBanker banker;
    const int64_t budget = 10'000;
    banker.setBudget("camp:s", MicroUSD(budget));
    banker.setBudget("camp", MicroUSD(budget));

    constexpr int threads = 8;
    constexpr int perThread = 5000;
    std::atomic<bool> overspent{false};

    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) {
        pool.emplace_back([&, t] {
            for (int i = 0; i < perThread; ++i) {
                std::string item = std::to_string(t) + "-" + std::to_string(i);
                if (banker.authorizeBid("camp:s", item, MicroUSD(3))) {
                    // settle some, cancel others, keep the rest in flight
                    if (i % 3 == 0) banker.winBid("camp:s", item, MicroUSD(2));
                    else if (i % 3 == 1) banker.cancelBid("camp:s", item);
                }
                AccountSummary s = banker.getAccountSummary("camp");
                if (s.spent + s.inFlight > MicroUSD(budget)) overspent = true;
            }
        });
    }
    for (auto& th : pool) th.join();

    CHECK_FALSE(overspent);
    AccountSummary s = banker.getAccountSummary("camp:s");
    CHECK(s.spent + s.inFlight <= MicroUSD(budget));
    CHECK(s.available().isNonNegative());
    // everything that is not cancelled is either spent (2) or in flight (3)
    CHECK(s.inFlight.value % 3 == 0);
    CHECK(s.spent.value % 2 == 0);
    CHECK(s.spent.value + s.inFlight.value <= budget);
    CHECK(banker.getAccountSummary("camp").spent == s.spent);
    CHECK(banker.getAccountSummary("camp").inFlight == s.inFlight);
}

TEST_CASE("in memory banker: concurrent saturation grants exactly the budget")
{
    InMemoryBanker banker;
    banker.setBudget("a", MicroUSD(1000));
    std::atomic<int> granted{0};
    std::vector<std::thread> pool;
    for (int t = 0; t < 8; ++t)
        pool.emplace_back([&, t] {
            for (int i = 0; i < 500; ++i)
                if (banker.authorizeBid("a", std::to_string(t) + "-" + std::to_string(i),
                                        MicroUSD(1)))
                    ++granted;
        });
    for (auto& th : pool) th.join();
    CHECK(granted == 1000);
    CHECK(banker.getAccountSummary("a").inFlight == MicroUSD(1000));
}

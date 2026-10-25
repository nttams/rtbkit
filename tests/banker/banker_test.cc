// Banker interface: NullBanker and the summary JSON. Real implementations add
// their own test files in this folder.

#include "banker/banker.h"

#include <catch2/catch_test_macros.hpp>

using namespace RTBKIT;
using nlohmann::json;

TEST_CASE("banker: account summary json")
{
    AccountSummary s;
    s.account = AccountKey("a:b");
    s.budget = USD_CPM(10);
    s.spent = USD_CPM(3);
    s.inFlight = USD_CPM(2);

    CHECK(s.available() == USD_CPM(5));

    json j = s.toJson();
    CHECK(j.at("available") == json(USD_CPM(5)));

    AccountSummary back = AccountSummary::fromJson(json::parse(j.dump()));
    CHECK(back.account == s.account);
    CHECK(back.budget == s.budget);
    CHECK(back.spent == s.spent);
    CHECK(back.inFlight == s.inFlight);

    CHECK_THROWS_AS(AccountSummary::fromJson(json{{"account", "a"}}), std::runtime_error);
}

TEST_CASE("banker: null banker authorizes everything")
{
    NullBanker banker;
    AccountKey account("a:b");

    CHECK(banker.authorizeBid(account, "item", USD_CPM(1000)));
    banker.winBid(account, "item", USD_CPM(1));
    banker.cancelBid(account, "other");
    banker.forceWinBid(account, "late", USD_CPM(1));
    banker.setBudget(account, USD_CPM(5));

    AccountSummary s = banker.getAccountSummary(account);
    CHECK(s.account == account);
    CHECK(s.budget.isZero());
    CHECK(banker.accounts().empty());
    CHECK(banker.summaryJson().at("accounts").empty());

    Banker& base = banker;   // usable through the interface
    CHECK(base.authorizeBid(account, "x", USD_CPM(1)));
}

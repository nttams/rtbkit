// Scenarios every Banker implementation must pass. Used by the in memory and
// the Redis tests.
#pragma once

#include "banker/banker.h"

#include <catch2/catch_test_macros.hpp>

namespace banker_test {

using namespace RTBKIT;

inline void checkBudgetAndHierarchy(Banker& banker)
{
    AccountKey leafA("camp:a"), leafB("camp:b"), parent("camp");
    banker.setBudget(leafA, MicroUSD(100));
    banker.setBudget(leafB, MicroUSD(50));

    // unknown account is refused, summary is zero
    CHECK_FALSE(banker.authorizeBid("nope:x", "i0", MicroUSD(1)));
    CHECK(banker.getAccountSummary("nope:x").budget.isZero());

    CHECK(banker.authorizeBid(leafA, "i1", MicroUSD(60)));
    CHECK_FALSE(banker.authorizeBid(leafA, "i2", MicroUSD(41)));   // 100 - 60
    CHECK(banker.authorizeBid(leafA, "i2", MicroUSD(40)));
    CHECK_FALSE(banker.authorizeBid(leafA, "i3", MicroUSD(1)));    // full
    CHECK_FALSE(banker.authorizeBid(leafA, "neg", MicroUSD(-1)));

    // the parent sums its children
    AccountSummary p = banker.getAccountSummary(parent);
    CHECK(p.budget == MicroUSD(150));
    CHECK(p.inFlight == MicroUSD(100));
    CHECK(p.spent.isZero());
    CHECK(p.available() == MicroUSD(50));

    // an account tree: parents exist, ordered output
    std::vector<AccountKey> all = banker.accounts();
    CHECK(all == std::vector<AccountKey>{parent, leafA, leafB});
    CHECK(banker.summaryJson().at("accounts").size() == 3);

    // a parent with its own budget caps the subtree
    banker.setBudget(parent, MicroUSD(110));
    CHECK(banker.getAccountSummary(parent).budget == MicroUSD(110));
    CHECK_FALSE(banker.authorizeBid(leafB, "b1", MicroUSD(11)));   // 100 used + 11 > 110
    CHECK(banker.authorizeBid(leafB, "b1", MicroUSD(10)));
    CHECK_FALSE(banker.authorizeBid(leafB, "b2", MicroUSD(1)));

    // leaf budget still applies even if the parent has room
    banker.setBudget(parent, MicroUSD(1000));
    CHECK_FALSE(banker.authorizeBid(leafB, "b3", MicroUSD(41)));
    CHECK(banker.authorizeBid(leafB, "b3", MicroUSD(40)));
}

inline void checkSettlement(Banker& banker)
{
    AccountKey leaf("c:s"), parent("c");
    banker.setBudget(leaf, MicroUSD(100));

    // cancel releases, unknown cancel is ignored
    CHECK(banker.authorizeBid(leaf, "x", MicroUSD(70)));
    banker.cancelBid(leaf, "x");
    banker.cancelBid(leaf, "x");
    banker.cancelBid(leaf, "never-seen");
    banker.cancelBid("unknown:acct", "x");
    CHECK(banker.getAccountSummary(leaf).inFlight.isZero());
    CHECK(banker.authorizeBid(leaf, "x", MicroUSD(100)));   // room again
    banker.cancelBid(leaf, "x");

    // idempotent authorize: same item twice is one reservation
    CHECK(banker.authorizeBid(leaf, "dup", MicroUSD(30)));
    CHECK(banker.authorizeBid(leaf, "dup", MicroUSD(30)));
    CHECK(banker.getAccountSummary(leaf).inFlight == MicroUSD(30));
    // replacing with a bigger amount that fits
    CHECK(banker.authorizeBid(leaf, "dup", MicroUSD(100)));
    CHECK(banker.getAccountSummary(leaf).inFlight == MicroUSD(100));
    // replacing with an amount that does not fit keeps the old one
    CHECK_FALSE(banker.authorizeBid(leaf, "dup", MicroUSD(101)));
    CHECK(banker.getAccountSummary(leaf).inFlight == MicroUSD(100));
    banker.cancelBid(leaf, "dup");

    // win under the reservation: the rest is released
    CHECK(banker.authorizeBid(leaf, "w1", MicroUSD(40)));
    banker.winBid(leaf, "w1", MicroUSD(25));
    AccountSummary s = banker.getAccountSummary(leaf);
    CHECK(s.spent == MicroUSD(25));
    CHECK(s.inFlight.isZero());
    CHECK(s.available() == MicroUSD(75));
    CHECK(banker.getAccountSummary(parent).spent == MicroUSD(25));

    // a repeated win, a cancel after the win and a new authorize are ignored
    banker.winBid(leaf, "w1", MicroUSD(25));
    banker.forceWinBid(leaf, "w1", MicroUSD(25));
    banker.cancelBid(leaf, "w1");
    CHECK_FALSE(banker.authorizeBid(leaf, "w1", MicroUSD(1)));
    CHECK(banker.getAccountSummary(leaf).spent == MicroUSD(25));

    // win over the reservation: paid is charged in full
    CHECK(banker.authorizeBid(leaf, "w2", MicroUSD(10)));
    banker.winBid(leaf, "w2", MicroUSD(15));
    s = banker.getAccountSummary(leaf);
    CHECK(s.spent == MicroUSD(40));
    CHECK(s.inFlight.isZero());

    // win of an unknown item charges like forceWinBid; may go over budget
    banker.winBid(leaf, "late", MicroUSD(30));
    CHECK(banker.getAccountSummary(leaf).spent == MicroUSD(70));
    banker.forceWinBid(leaf, "late2", MicroUSD(50));
    s = banker.getAccountSummary(leaf);
    CHECK(s.spent == MicroUSD(120));
    CHECK(s.available() == MicroUSD(-20));
    CHECK_FALSE(banker.authorizeBid(leaf, "after", MicroUSD(1)));

    // forceWinBid on a reserved item releases the reservation
    banker.setBudget(leaf, MicroUSD(1000));
    CHECK(banker.authorizeBid(leaf, "f1", MicroUSD(100)));
    banker.forceWinBid(leaf, "f1", MicroUSD(90));
    s = banker.getAccountSummary(leaf);
    CHECK(s.inFlight.isZero());
    CHECK(s.spent == MicroUSD(210));

    // force win on an unknown account creates it, parents included
    banker.forceWinBid("fresh:one", "z", MicroUSD(5));
    CHECK(banker.getAccountSummary("fresh:one").spent == MicroUSD(5));
    CHECK(banker.getAccountSummary("fresh").spent == MicroUSD(5));
    CHECK(banker.getAccountSummary("fresh").budget.isZero());

    // raising the budget keeps spent and in-flight
    CHECK(banker.authorizeBid(leaf, "keep", MicroUSD(10)));
    banker.setBudget(leaf, MicroUSD(2000));
    s = banker.getAccountSummary(leaf);
    CHECK(s.budget == MicroUSD(2000));
    CHECK(s.spent == MicroUSD(210));
    CHECK(s.inFlight == MicroUSD(10));

    CHECK_THROWS_AS(banker.setBudget(leaf, MicroUSD(-1)), std::invalid_argument);
    CHECK_THROWS_AS(banker.winBid(leaf, "neg", MicroUSD(-1)), std::invalid_argument);
}

} // namespace banker_test

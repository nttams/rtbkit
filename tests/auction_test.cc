// Auction, Bid, Bids, Response and WinCostModel.

#include "common/auction.h"

#include "fixtures.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace RTBKIT;
using namespace RTBKIT::Test;
using nlohmann::json;

TEST_CASE("bid / bids / response: JSON round trip")
{
    Bid bid;
    bid.spotIndex = 1;
    bid.creativeIndex = 2;
    bid.price = USD_CPM(1.25);
    bid.priority = 3;
    bid.account = AccountKey("a:b");
    bid.bidData = "xyz";
    bid.meta = json{{"k", 1}};
    REQUIRE(bid.valid());

    Bid back = Bid::fromJson(bid.toJson());
    CHECK(back.spotIndex == 1);
    CHECK(back.creativeIndex == 2);
    CHECK(back.price == USD_CPM(1.25));
    CHECK(back.priority == 3);
    CHECK(back.account == AccountKey("a:b"));
    CHECK(back.bidData == "xyz");
    CHECK(back.meta == bid.meta);

    Bids bids;
    bids.push_back(bid);
    bids.push_back(Bid{0, 0, USD_CPM(2), 0, {}, "", {}});
    bids.meta = json{{"m", true}};
    Bids bidsBack = Bids::fromJson(bids.toJson());
    REQUIRE(bidsBack.size() == 2);
    CHECK(bidsBack[1].price == USD_CPM(2));
    CHECK(bidsBack.meta == bids.meta);
    CHECK(bidsBack.toJson() == bids.toJson());

    // A bare array is accepted too.
    CHECK(Bids::fromJson(bids.toJson()["bids"]).size() == 2);

    Response r = sampleResponse(1, 1.5);
    r.meta = json{{"x", "y"}};
    Response rBack = Response::fromJson(r.toJson());
    CHECK(rBack.toJson() == r.toJson());
    CHECK(rBack.agent == "agent1");
    CHECK(rBack.price == USD_CPM(1.5));
}

TEST_CASE("bid: invalid JSON and values")
{
    CHECK_THROWS_AS(Bid::fromJson(json::array()), std::runtime_error);
    CHECK_THROWS_AS(Bid::fromJson(json{{"spot", 0}}), std::runtime_error);
    CHECK_THROWS_AS(Bids::fromJson(json{{"nope", 1}}), std::runtime_error);
    CHECK_THROWS_AS(Response::fromJson(json{{"spot", "x"}}), std::runtime_error);

    Bid bid;
    CHECK_FALSE(bid.valid());
    bid.spotIndex = 0;
    bid.creativeIndex = 0;
    CHECK_FALSE(bid.valid());   // zero price
    bid.price = USD_CPM(1);
    CHECK(bid.valid());
}

TEST_CASE("auction: best response by priority then price")
{
    auto auction = Auction::create(sampleBidRequest());
    REQUIRE(auction->numSpots() == 2);
    CHECK(auction->id() == Id("auction-1"));

    CHECK(auction->setResponse(sampleResponse(0, 1.0, "a")) == Auction::SetResult::Best);
    CHECK(auction->setResponse(sampleResponse(0, 2.0, "b")) == Auction::SetResult::Best);
    CHECK(auction->setResponse(sampleResponse(0, 1.5, "c")) == Auction::SetResult::Outbid);
    CHECK(auction->setResponse(sampleResponse(0, 2.0, "d")) == Auction::SetResult::Outbid);

    CHECK(auction->bestResponse(0)->agent == "b");
    CHECK(auction->secondPrice(0) == USD_CPM(2.0));   // "d" tied the winner
    CHECK(auction->numResponses(0) == 4);

    // Higher priority beats a higher price.
    CHECK(auction->setResponse(sampleResponse(0, 0.6, "e", 5)) == Auction::SetResult::Best);
    CHECK(auction->bestResponse(0)->agent == "e");

    CHECK_FALSE(auction->bestResponse(1).has_value());
    CHECK(auction->hasResponses());
}

TEST_CASE("auction: rejected responses")
{
    auto auction = Auction::create(sampleBidRequest());
    CHECK(auction->setResponse(sampleResponse(5, 1.0)) == Auction::SetResult::InvalidSpot);
    CHECK(auction->setResponse(sampleResponse(-1, 1.0)) == Auction::SetResult::InvalidSpot);
    CHECK(auction->setResponse(sampleResponse(0, 0.0)) == Auction::SetResult::BadPrice);

    Response eur = sampleResponse(0, 1.0);
    CHECK(auction->setResponse(sampleResponse(0, 1.0)) == Auction::SetResult::Best);
    eur.price = EUR_CPM(1.0);
    CHECK(auction->setResponse(eur) == Auction::SetResult::BadPrice);

    CHECK(auction->finish());
    CHECK(auction->setResponse(sampleResponse(0, 9.0)) == Auction::SetResult::Finished);
    CHECK(auction->bestResponse(0)->price == USD_CPM(1.0));
}

TEST_CASE("auction: finish runs the completion once")
{
    int calls = 0;
    std::shared_ptr<Auction> seen;
    auto auction = Auction::create(sampleBidRequest(), [&](std::shared_ptr<Auction> a) {
        ++calls;
        seen = a;
    });

    CHECK_FALSE(auction->finished());
    CHECK(auction->finish());
    CHECK_FALSE(auction->finish());
    CHECK(auction->finished());
    CHECK(calls == 1);
    CHECK(seen == auction);

    // No completion is fine.
    CHECK(Auction::create(sampleBidRequest())->finish());
    CHECK_THROWS_AS(Auction::create(nullptr), std::invalid_argument);
}

TEST_CASE("auction: expiry follows timeAvailableMs")
{
    using namespace std::chrono_literals;
    auto start = Auction::Clock::now();
    auto auction = Auction::create(sampleBidRequest(), {}, start);

    CHECK(auction->start() == start);
    CHECK(auction->expiry() - start == 100ms);
    CHECK_FALSE(auction->expired(start + 99ms));
    CHECK(auction->expired(start + 100ms));
    CHECK(auction->timeLeftMs(start + 40ms) == Catch::Approx(60.0));
}

TEST_CASE("win cost model: first and second price")
{
    WinCostModel first;
    first.type = WinCostModel::Type::FirstPrice;
    WinCostModel second;

    CHECK(first.evaluate(USD_CPM(3), USD_CPM(1), USD_CPM(0.5)) == USD_CPM(3));
    CHECK(second.evaluate(USD_CPM(3), USD_CPM(1), USD_CPM(0.5)) == USD_CPM(1));
    // The reserve is the floor of the price...
    CHECK(second.evaluate(USD_CPM(3), Amount(), USD_CPM(0.5)) == USD_CPM(0.5));
    // ...and the bid its ceiling.
    CHECK(second.evaluate(USD_CPM(1), USD_CPM(2), Amount()) == USD_CPM(1));

    auto request = sampleBidRequest();
    CHECK(WinCostModel::forRequest(*request).type == WinCostModel::Type::SecondPrice);
    request->auctionType = AuctionType::FirstPrice;
    CHECK(WinCostModel::forRequest(*request).type == WinCostModel::Type::FirstPrice);

    auto auction = Auction::create(sampleBidRequest());
    auction->setResponse(sampleResponse(0, 3.0, "a"));
    auction->setResponse(sampleResponse(0, 2.0, "b"));
    CHECK(second.evaluate(*auction, 0) == USD_CPM(2.0));
    CHECK(first.evaluate(*auction, 0) == USD_CPM(3.0));
    CHECK(second.evaluate(*auction, 1) == Amount());   // no response

    CHECK(WinCostModel::fromJson(second.toJson()).type == WinCostModel::Type::SecondPrice);
    CHECK(WinCostModel::fromJson(first.toJson()).type == WinCostModel::Type::FirstPrice);
    CHECK_THROWS_AS(WinCostModel::fromJson(json{{"type", "third"}}), std::runtime_error);
}

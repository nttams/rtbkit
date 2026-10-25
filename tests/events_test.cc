// SubmittedAuctionEvent, PostAuctionEvent, MatchedWinLoss, MatchedCampaignEvent.

#include "common/events.h"

#include "fixtures.h"

#include <catch2/catch_test_macros.hpp>

using namespace RTBKIT;
using namespace RTBKIT::Test;
using nlohmann::json;

TEST_CASE("events: submitted auction event round trip")
{
    SubmittedAuctionEvent e;
    e.auctionId = Id("auction-1");
    e.adSpotId = Id("2");
    e.lossTimeout = dateFromMs(1'700'000'000'000);
    e.bid = sampleResponse(1, 2.5);
    e.request = sampleBidRequest();

    SubmittedAuctionEvent back = SubmittedAuctionEvent::fromJson(json::parse(e.toJson().dump()));
    CHECK(back.toJson() == e.toJson());
    CHECK(back.auctionId == Id("auction-1"));
    CHECK(back.adSpotId == Id("2"));
    CHECK(dateToMs(back.lossTimeout) == 1'700'000'000'000);
    CHECK(back.bid.price == USD_CPM(2.5));
    REQUIRE(back.request);
    CHECK(back.request->imp.size() == 2);

    e.request.reset();
    CHECK_FALSE(SubmittedAuctionEvent::fromJson(e.toJson()).request);
}

TEST_CASE("events: post auction events")
{
    PostAuctionEvent win;
    win.type = PostAuctionEventType::WIN;
    win.auctionId = Id("a");
    win.adSpotId = Id("1");
    win.timestamp = dateFromMs(5000);
    win.winPrice = USD_CPM(1.2);
    PostAuctionEvent winBack = PostAuctionEvent::fromJson(win.toJson());
    CHECK(winBack.type == PostAuctionEventType::WIN);
    CHECK(winBack.winPrice == USD_CPM(1.2));
    CHECK(winBack.toJson() == win.toJson());

    PostAuctionEvent click;
    click.type = PostAuctionEventType::CAMPAIGN_EVENT;
    click.campaignEvent = CampaignEventType::CLICK;
    click.auctionId = Id("a");
    click.adSpotId = Id("1");
    click.meta = json{{"x", 1}};
    PostAuctionEvent clickBack = PostAuctionEvent::fromJson(click.toJson());
    CHECK(clickBack.campaignEvent == CampaignEventType::CLICK);
    CHECK(clickBack.meta == click.meta);

    PostAuctionEvent loss;
    loss.type = PostAuctionEventType::LOSS;
    CHECK(PostAuctionEvent::fromJson(loss.toJson()).type == PostAuctionEventType::LOSS);

    CHECK(toString(PostAuctionEventType::CAMPAIGN_EVENT) == "CAMPAIGN_EVENT");
    CHECK(toString(CampaignEventType::IMPRESSION) == "IMPRESSION");
    CHECK_THROWS_AS(parsePostAuctionEventType("NOPE"), std::runtime_error);
    CHECK_THROWS_AS(PostAuctionEvent::fromJson(json{{"type", "WIN"}}), std::runtime_error);
}

TEST_CASE("events: matched win / loss / campaign event")
{
    MatchedWinLoss m;
    m.type = MatchedWinLoss::Type::LATE_WIN;
    m.auctionId = Id("a");
    m.adSpotId = Id("1");
    m.timestamp = dateFromMs(42);
    m.winPrice = USD_CPM(0.9);
    m.bid = sampleResponse(0, 1.0);

    MatchedWinLoss back = MatchedWinLoss::fromJson(m.toJson());
    CHECK(back.type == MatchedWinLoss::Type::LATE_WIN);
    CHECK(back.winPrice == USD_CPM(0.9));
    CHECK(back.bid.bidData == "data-agent1");
    CHECK(back.toJson() == m.toJson());

    for (auto type : {MatchedWinLoss::Type::WIN, MatchedWinLoss::Type::LOSS})
        CHECK(parseMatchedWinLossType(toString(type)) == type);

    MatchedCampaignEvent c;
    c.event = CampaignEventType::CLICK;
    c.auctionId = Id("a");
    c.adSpotId = Id("1");
    c.bid = sampleResponse(0, 1.0);
    CHECK(MatchedCampaignEvent::fromJson(c.toJson()).toJson() == c.toJson());
    CHECK(MatchedCampaignEvent::fromJson(c.toJson()).event == CampaignEventType::CLICK);
}

TEST_CASE("events: banker item id")
{
    CHECK(bidItemId(Id("a"), Id("1"), "bidder") == "a:1:bidder");
    CHECK(bidItemId(Id("a"), Id("1"), "x") != bidItemId(Id("a"), Id("1"), "y"));
}

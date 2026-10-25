// Router <-> agent wire protocol: encode / decode round trips, no sockets.

#include "agent/protocol.h"

#include "fixtures.h"

#include <catch2/catch_test_macros.hpp>

using namespace RTBKIT;
using namespace RTBKIT::protocol;
using namespace RTBKIT::Test;
using nlohmann::json;

namespace {

Message roundTrip(const Message& message)
{
    Frames frames = encode(message);
    REQUIRE_FALSE(frames.empty());
    CHECK(frames[0] == toString(typeOf(message)));
    return decode(frames);
}

MatchedWinLoss sampleMatch(MatchedWinLoss::Type type)
{
    MatchedWinLoss m;
    m.type = type;
    m.auctionId = Id("auction-1");
    m.adSpotId = Id("1");
    m.timestamp = dateFromMs(1234);
    m.winPrice = type == MatchedWinLoss::Type::LOSS ? Amount() : USD_CPM(0.8);
    m.bid = sampleResponse(0, 1.0);
    return m;
}

} // namespace

TEST_CASE("protocol: CONFIG, GOTCONFIG, NEEDCONFIG")
{
    AgentConfig config = sampleAgentConfig("agent1", "a:b");
    auto msg = std::get<ConfigMsg>(roundTrip(ConfigMsg{"agent1", config}));
    CHECK(msg.agent == "agent1");
    CHECK(msg.config.toJson() == config.toJson());

    CHECK(std::get<GotConfigMsg>(roundTrip(GotConfigMsg{"agent1"})).agent == "agent1");
    CHECK(std::holds_alternative<NeedConfigMsg>(roundTrip(NeedConfigMsg{})));
}

TEST_CASE("protocol: AUCTION")
{
    AuctionMsg m;
    m.auctionId = Id("auction-1");
    m.timeLeftMs = 87.25;
    m.request = sampleBidRequest();
    m.spots = {{0, {0}}, {1, {0, 2}}};

    Frames frames = encode(m);
    REQUIRE(frames.size() == 5);
    CHECK(frames[0] == "AUCTION");
    CHECK(frames[1] == "auction-1");

    auto back = std::get<AuctionMsg>(decode(frames));
    CHECK(back.auctionId == Id("auction-1"));
    CHECK(back.timeLeftMs == 87.25);
    REQUIRE(back.request);
    CHECK(toJson(*back.request) == toJson(*m.request));
    CHECK(back.spots == m.spots);

    m.request.reset();
    CHECK_THROWS_AS(encode(m), ProtocolError);
}

TEST_CASE("protocol: BID")
{
    BidMsg m;
    m.auctionId = Id("auction-1");
    m.bids.push_back(Bid{1, 0, USD_CPM(1.5), 2, AccountKey("x:y"), "data", {}});
    m.bids.push_back(Bid{0, 0, USD_CPM(2.5), 0, {}, "", {}});

    auto back = std::get<BidMsg>(roundTrip(m));
    CHECK(back.auctionId == Id("auction-1"));
    CHECK(back.bids.toJson() == m.bids.toJson());

    // A pass is a BID with no bids.
    BidMsg pass{Id("auction-1"), {}};
    CHECK(std::get<BidMsg>(roundTrip(pass)).bids.empty());
}

TEST_CASE("protocol: router replies")
{
    CHECK(std::get<NoBudgetMsg>(roundTrip(NoBudgetMsg{Id("a")})).auctionId == Id("a"));
    CHECK(std::get<TooLateMsg>(roundTrip(TooLateMsg{Id("a")})).auctionId == Id("a"));

    auto invalid = std::get<InvalidMsg>(roundTrip(InvalidMsg{Id("a"), "bad spot"}));
    CHECK(invalid.auctionId == Id("a"));
    CHECK(invalid.reason == "bad spot");

    auto error = std::get<ErrorMsg>(roundTrip(ErrorMsg{"nope", Id()}));
    CHECK(error.reason == "nope");
    CHECK(error.auctionId.empty());
    CHECK(std::get<ErrorMsg>(roundTrip(ErrorMsg{"nope", Id("a")})).auctionId == Id("a"));
}

TEST_CASE("protocol: WIN, LOSS, CAMPAIGN_EVENT")
{
    auto win = std::get<WinMsg>(roundTrip(WinMsg{sampleMatch(MatchedWinLoss::Type::WIN)}));
    CHECK(win.event.type == MatchedWinLoss::Type::WIN);
    CHECK(win.event.winPrice == USD_CPM(0.8));
    CHECK(win.event.bid.bidData == "data-agent1");

    auto loss = std::get<LossMsg>(roundTrip(LossMsg{sampleMatch(MatchedWinLoss::Type::LOSS)}));
    CHECK(loss.event.type == MatchedWinLoss::Type::LOSS);
    CHECK(loss.event.winPrice.isZero());

    MatchedCampaignEvent c;
    c.event = CampaignEventType::CLICK;
    c.auctionId = Id("auction-1");
    c.adSpotId = Id("1");
    c.bid = sampleResponse(0, 1.0);
    auto click = std::get<CampaignEventMsg>(roundTrip(CampaignEventMsg{c}));
    CHECK(click.event.event == CampaignEventType::CLICK);
    CHECK(click.event.toJson() == c.toJson());
}

TEST_CASE("protocol: PING / PONG")
{
    CHECK(std::get<PingMsg>(roundTrip(PingMsg{"t1"})).token == "t1");
    CHECK(std::get<PongMsg>(roundTrip(PongMsg{"t1"})).token == "t1");
}

TEST_CASE("protocol: message type names")
{
    for (auto type : {MessageType::CONFIG, MessageType::GOTCONFIG, MessageType::NEEDCONFIG,
                      MessageType::AUCTION, MessageType::BID, MessageType::NOBUDGET,
                      MessageType::TOOLATE, MessageType::INVALID, MessageType::ERROR,
                      MessageType::WIN, MessageType::LOSS, MessageType::CAMPAIGN_EVENT,
                      MessageType::PING, MessageType::PONG})
        CHECK(parseMessageType(toString(type)) == type);
    CHECK_THROWS_AS(parseMessageType("HELLO"), ProtocolError);
}

TEST_CASE("protocol: malformed messages")
{
    CHECK_THROWS_AS(decode({}), ProtocolError);
    CHECK_THROWS_AS(decode({"HELLO"}), ProtocolError);
    CHECK_THROWS_AS(decode({"BID", "a"}), ProtocolError);               // frame count
    CHECK_THROWS_AS(decode({"BID", "a", "not json"}), ProtocolError);
    CHECK_THROWS_AS(decode({"BID", "a", "{\"bids\": 3}"}), ProtocolError);
    CHECK_THROWS_AS(decode({"CONFIG", "a", "{}"}), ProtocolError);      // no account
    CHECK_THROWS_AS(decode({"AUCTION", "a", "x", "{}", "[]"}), ProtocolError);   // bad time
    CHECK_THROWS_AS(decode({"AUCTION", "a", "5", "[]", "[]"}), ProtocolError);   // bad request
    CHECK_THROWS_AS(decode({"WIN", "{}"}), ProtocolError);
    CHECK_THROWS_AS(decode({"NEEDCONFIG", "extra"}), ProtocolError);
}

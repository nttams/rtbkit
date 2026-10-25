// BiddingAgent against a hand-driven ROUTER socket (no real router).

#include "fixtures.h"

#include "agent/bidding_agent.h"
#include "agent/zmq_util.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

using namespace RTBKIT;
using namespace RTBKIT::Test;
using namespace std::chrono_literals;

namespace {

/** Plays the router: binds a ROUTER socket and talks to one agent. */
struct FakeRouter {
    FakeRouter() : socket(context, zmq::socket_type::router)
    {
        socket.set(zmq::sockopt::linger, 0);
        socket.set(zmq::sockopt::rcvtimeo, 2000);
        socket.bind("tcp://127.0.0.1:*");
        endpoint = socket.get(zmq::sockopt::last_endpoint);
    }

    /** Next message from the agent, decoded; also remembers the identity. */
    protocol::Message receive()
    {
        std::vector<std::string> frames;
        REQUIRE(zmqutil::recvFramesWait(socket, frames, 2000ms));
        REQUIRE(frames.size() >= 2);
        identity = frames[0];
        return protocol::decode(protocol::Frames(frames.begin() + 1, frames.end()));
    }

    void send(const protocol::Message& message)
    {
        auto frames = protocol::encode(message);
        frames.insert(frames.begin(), identity);
        zmqutil::sendFrames(socket, frames);
    }

    zmq::context_t context;
    zmq::socket_t socket;
    std::string endpoint;
    std::string identity;
};

template<typename Predicate>
bool waitUntil(Predicate predicate)
{
    for (int i = 0; i < 1000; ++i) {
        if (predicate()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return predicate();
}

protocol::AuctionMsg auctionMsg(const std::string& id)
{
    protocol::AuctionMsg msg;
    msg.auctionId = Id(id);
    msg.timeLeftMs = 80;
    msg.request = sampleBidRequest(id);
    msg.spots = {{0, {0}}, {1, {0}}};
    return msg;
}

} // namespace

TEST_CASE("bidding agent: handshake auction bid and events")
{
    FakeRouter router;
    BiddingAgent agent("agent-x", sampleAgentConfig("agent-x"), router.endpoint);

    std::atomic<int> wins{0}, losses{0}, clicks{0}, nobudget{0}, toolate{0}, invalid{0};
    std::atomic<int> gotConfig{0}, errors{0};
    std::mutex mutex;
    std::vector<protocol::AuctionSpot> seenSpots;
    double seenTimeLeft = 0;
    std::string seenUrl;

    agent.onGotConfig = [&] { ++gotConfig; };
    agent.onBidRequest = [&](const Id& id, std::shared_ptr<const BidRequest> request,
                             const std::vector<protocol::AuctionSpot>& spots, double timeLeft) {
        {
            std::lock_guard lock(mutex);
            seenSpots = spots;
            seenTimeLeft = timeLeft;
            seenUrl = request->url.toString();
        }
        Bids bids;
        Bid bid;
        bid.spotIndex = spots.at(0).spotIndex;
        bid.creativeIndex = int(spots.at(0).creatives.at(0));
        bid.price = USD_CPM(2.0);
        bid.bidData = "x";
        bids.push_back(bid);
        agent.doBid(id, bids);
    };
    agent.onWin = [&](const MatchedWinLoss&) { ++wins; };
    agent.onLoss = [&](const MatchedWinLoss&) { ++losses; };
    agent.onCampaignEvent = [&](const MatchedCampaignEvent&) { ++clicks; };
    agent.onNoBudget = [&](const Id&) { ++nobudget; };
    agent.onTooLate = [&](const Id&) { ++toolate; };
    agent.onInvalid = [&](const Id&, const std::string&) { ++invalid; };
    agent.onError = [&](const std::string&, const Id&) { ++errors; };
    agent.start();

    // CONFIG with the agent name and a config that survives the round trip.
    auto config = router.receive();
    REQUIRE(std::holds_alternative<protocol::ConfigMsg>(config));
    CHECK(router.identity == "agent-x");
    CHECK(std::get<protocol::ConfigMsg>(config).agent == "agent-x");
    CHECK(std::get<protocol::ConfigMsg>(config).config.account == AccountKey("campaign:strategy"));
    CHECK_FALSE(agent.hasConfig());
    router.send(protocol::GotConfigMsg{"agent-x"});
    REQUIRE(agent.waitForConfig(2000ms));
    CHECK(agent.hasConfig());
    CHECK(gotConfig == 1);

    // AUCTION -> callback -> BID.
    router.send(auctionMsg("a1"));
    auto bid = router.receive();
    REQUIRE(std::holds_alternative<protocol::BidMsg>(bid));
    const auto& bidMsg = std::get<protocol::BidMsg>(bid);
    CHECK(bidMsg.auctionId == Id("a1"));
    REQUIRE(bidMsg.bids.size() == 1);
    CHECK(bidMsg.bids[0].price == USD_CPM(2.0));
    CHECK(bidMsg.bids[0].bidData == "x");
    {
        std::lock_guard lock(mutex);
        CHECK(seenSpots.size() == 2);
        CHECK(seenTimeLeft == 80);
        CHECK(seenUrl == "http://www.example.com/page");
    }

    // PING -> PONG.
    router.send(protocol::PingMsg{"tok"});
    auto pong = router.receive();
    REQUIRE(std::holds_alternative<protocol::PongMsg>(pong));
    CHECK(std::get<protocol::PongMsg>(pong).token == "tok");

    // Events reach their callbacks.
    MatchedWinLoss event;
    event.auctionId = Id("a1");
    event.bid = sampleResponse(0, 2.0, "agent-x");
    router.send(protocol::WinMsg{event});
    router.send(protocol::LossMsg{event});
    MatchedCampaignEvent click;
    click.bid = event.bid;
    router.send(protocol::CampaignEventMsg{click});
    router.send(protocol::NoBudgetMsg{Id("a1")});
    router.send(protocol::TooLateMsg{Id("a1")});
    router.send(protocol::InvalidMsg{Id("a1"), "why"});
    router.send(protocol::ErrorMsg{"boom", Id()});
    REQUIRE(waitUntil([&] {
        return wins == 1 && losses == 1 && clicks == 1 && nobudget == 1 && toolate == 1
            && invalid == 1 && errors == 1;
    }));

    // NEEDCONFIG: the agent sends CONFIG again.
    router.send(protocol::NeedConfigMsg{});
    CHECK(std::holds_alternative<protocol::ConfigMsg>(router.receive()));

    // doBid from another thread, and from before any auction callback.
    Bids none;
    std::thread([&] { agent.doBid(Id("a2"), none); }).join();
    auto empty = router.receive();
    REQUIRE(std::holds_alternative<protocol::BidMsg>(empty));
    CHECK(std::get<protocol::BidMsg>(empty).auctionId == Id("a2"));
    CHECK(std::get<protocol::BidMsg>(empty).bids.empty());

    agent.stop();
    agent.stop();   // twice is fine
}

TEST_CASE("bidding agent: an AUCTION nobody handles or that throws is passed")
{
    FakeRouter router;
    BiddingAgent agent("agent-y", sampleAgentConfig("agent-y"), router.endpoint);
    std::atomic<int> errors{0};
    agent.onError = [&](const std::string&, const Id&) { ++errors; };
    agent.start();
    REQUIRE(std::holds_alternative<protocol::ConfigMsg>(router.receive()));

    // No callback set.
    router.send(auctionMsg("n1"));
    auto reply = router.receive();
    REQUIRE(std::holds_alternative<protocol::BidMsg>(reply));
    CHECK(std::get<protocol::BidMsg>(reply).auctionId == Id("n1"));
    CHECK(std::get<protocol::BidMsg>(reply).bids.empty());
    agent.stop();

    // Callback that throws.
    BiddingAgent thrower("agent-z", sampleAgentConfig("agent-z"), router.endpoint);
    thrower.onError = [&](const std::string&, const Id&) { ++errors; };
    thrower.onBidRequest = [](const Id&, std::shared_ptr<const BidRequest>,
                              const std::vector<protocol::AuctionSpot>&, double) {
        throw std::runtime_error("bug in the agent");
    };
    thrower.start();
    REQUIRE(std::holds_alternative<protocol::ConfigMsg>(router.receive()));
    router.send(auctionMsg("t1"));
    reply = router.receive();
    REQUIRE(std::holds_alternative<protocol::BidMsg>(reply));
    CHECK(std::get<protocol::BidMsg>(reply).auctionId == Id("t1"));
    CHECK(std::get<protocol::BidMsg>(reply).bids.empty());
    REQUIRE(waitUntil([&] { return errors == 1; }));
}

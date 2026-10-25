// Router: config JSON, and the whole flow over real zmq sockets with real
// BiddingAgent clients, a fake exchange connector and a recording banker.

#include "fixtures.h"

#include "agent/bidding_agent.h"
#include "agent/protocol.h"
#include "agent/zmq_util.h"
#include "banker/banker.h"
#include "router/router.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <set>
#include <thread>

using namespace RTBKIT;
using namespace RTBKIT::Test;
using nlohmann::json;
using namespace std::chrono_literals;

namespace {

template<typename Predicate>
bool waitUntil(Predicate predicate, std::chrono::milliseconds limit = 2000ms)
{
    const auto end = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < end) {
        if (predicate()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return predicate();
}

/** Connector that lets the test submit auctions. */
struct FakeExchange : public ExchangeConnector {
    FakeExchange() : ExchangeConnector("test-exchange") {}
    void configure(const json&) override {}
    void start() override {}
    void stop() override {}
    void inject(std::shared_ptr<Auction> auction) { submit(std::move(auction)); }
};

/** Authorizes everything except accounts under "poor"; records the calls. */
struct RecordingBanker : public NullBanker {
    bool authorizeBid(const AccountKey& account, const std::string& item,
                      const Amount&) override
    {
        std::lock_guard lock(mutex);
        authorized.insert(account.toString() + "|" + item);
        return !account.hasPrefix(AccountKey("poor"));
    }

    void cancelBid(const AccountKey& account, const std::string& item) override
    {
        std::lock_guard lock(mutex);
        cancelled.insert(account.toString() + "|" + item);
    }

    std::set<std::string> authorizedItems() { std::lock_guard l(mutex); return authorized; }
    std::set<std::string> cancelledItems() { std::lock_guard l(mutex); return cancelled; }

    std::mutex mutex;
    std::set<std::string> authorized, cancelled;
};

/** A BiddingAgent that records what it hears and bids `cpm` on spot 0. */
struct TestAgent {
    TestAgent(const std::string& name, const std::string& endpoint, double bidCpm,
              AgentConfig config)
        : cpm(bidCpm), agent(name, std::move(config), endpoint)
    {
        agent.onBidRequest = [this](const Id& id, std::shared_ptr<const BidRequest>,
                                    const std::vector<protocol::AuctionSpot>&, double) {
            ++auctions;
            { std::lock_guard l(mutex); heard.push_back(id); }
            if (silent) return;
            Bids bids;
            if (this->cpm > 0) {
                Bid bid;
                bid.spotIndex = 0;
                bid.creativeIndex = creative;
                bid.price = USD_CPM(this->cpm);
                bid.bidData = "bid-data";
                bids.push_back(bid);
            }
            agent.doBid(id, bids);
        };
        agent.onWin = [this](const MatchedWinLoss& e) { ++wins; lastWinPrice = e.winPrice; };
        agent.onLoss = [this](const MatchedWinLoss&) { ++losses; };
        agent.onCampaignEvent = [this](const MatchedCampaignEvent&) { ++campaignEvents; };
        agent.onNoBudget = [this](const Id&) { ++nobudget; };
        agent.onTooLate = [this](const Id&) { ++toolate; };
        agent.onInvalid = [this](const Id&, const std::string& reason) {
            ++invalid;
            std::lock_guard l(mutex);
            invalidReason = reason;
        };
        agent.start();
    }

    std::string reason() { std::lock_guard l(mutex); return invalidReason; }
    std::vector<Id> heardAuctions() { std::lock_guard l(mutex); return heard; }

    double cpm;
    int creative = 0;
    std::atomic<bool> silent{false};
    std::atomic<int> auctions{0}, wins{0}, losses{0}, campaignEvents{0}, nobudget{0},
        toolate{0}, invalid{0};
    Amount lastWinPrice;
    std::mutex mutex;
    std::vector<Id> heard;
    std::string invalidReason;
    BiddingAgent agent;
};

struct Submitted {
    std::shared_ptr<Auction> auction;
    std::shared_future<void> done;
    std::chrono::steady_clock::time_point started;

    bool waitDone(std::chrono::milliseconds limit = 2000ms) const
    {
        return done.wait_for(limit) == std::future_status::ready;
    }
};

struct Env {
    explicit Env(RouterConfig config = fastConfig())
        : banker(std::make_shared<RecordingBanker>()),
          exchange(std::make_shared<FakeExchange>()),
          router(std::move(config), banker)
    {
        router.addExchange(exchange);
        router.setSubmittedAuctionSink([this](const SubmittedAuctionEvent& event) {
            std::lock_guard lock(mutex);
            events.push_back(event);
        });
        router.start();
    }

    static RouterConfig fastConfig()
    {
        RouterConfig config;
        config.agentEndpoint = "tcp://127.0.0.1:*";
        config.pingIntervalMs = 40;
        config.agentTimeoutMs = 400;
        return config;
    }

    std::unique_ptr<TestAgent> addAgent(const std::string& name, const std::string& account,
                                        double cpm)
    {
        auto agent = std::make_unique<TestAgent>(
            name, router.boundEndpoint(), cpm, sampleAgentConfig(name, account));
        REQUIRE(agent->agent.waitForConfig(2000ms));
        return agent;
    }

    Submitted submit(const std::string& id, double timeAvailableMs = 300)
    {
        auto request = sampleBidRequest(id);
        request->timeAvailableMs = timeAvailableMs;
        auto promise = std::make_shared<std::promise<void>>();
        Submitted result;
        result.done = promise->get_future().share();
        result.started = std::chrono::steady_clock::now();
        result.auction = Auction::create(
            request, [promise](std::shared_ptr<Auction>) { promise->set_value(); });
        exchange->inject(result.auction);
        return result;
    }

    std::vector<SubmittedAuctionEvent> submittedEvents()
    {
        std::lock_guard lock(mutex);
        return events;
    }

    std::shared_ptr<RecordingBanker> banker;
    std::shared_ptr<FakeExchange> exchange;
    Router router;
    std::mutex mutex;
    std::vector<SubmittedAuctionEvent> events;
};

} // namespace

TEST_CASE("router: config json")
{
    RouterConfig defaults;
    CHECK(defaults.agentEndpoint == "tcp://127.0.0.1:12340");

    RouterConfig config = RouterConfig::fromJson(json{{"agentEndpoint", "tcp://127.0.0.1:5555"},
                                                      {"pingIntervalMs", 10},
                                                      {"agentTimeoutMs", 20},
                                                      {"lossTimeoutMs", 30}});
    CHECK(config.agentEndpoint == "tcp://127.0.0.1:5555");
    CHECK(config.pingIntervalMs == 10);
    RouterConfig again = RouterConfig::fromJson(config.toJson());
    CHECK(again.agentEndpoint == config.agentEndpoint);
    CHECK(again.agentTimeoutMs == 20);
    CHECK(again.lossTimeoutMs == 30);

    CHECK_THROWS_AS(RouterConfig::fromJson(json{{"agentEndpoint", 5}}), std::runtime_error);
    CHECK_THROWS_AS(RouterConfig::fromJson(json{{"pingIntervalMs", "x"}}), std::runtime_error);
}

TEST_CASE("router: best of two agents wins and the other is cancelled and told")
{
    Env env;
    auto low = env.addAgent("low", "acct:low", 2.0);
    auto high = env.addAgent("high", "acct:high", 3.0);

    auto run = env.submit("auction-w", 1000);
    REQUIRE(run.waitDone());
    // Both answered: no need to wait for the 1 second expiry.
    CHECK(std::chrono::steady_clock::now() - run.started < 500ms);

    auto best = run.auction->bestResponse(0);
    REQUIRE(best);
    CHECK(best->agent == "high");
    CHECK(best->price == USD_CPM(3.0));
    CHECK(best->account == AccountKey("acct:high"));
    CHECK(best->bidData == "bid-data");
    REQUIRE(best->config);
    CHECK(best->config->name == "high");
    CHECK_FALSE(run.auction->bestResponse(1));

    auto events = env.submittedEvents();
    REQUIRE(events.size() == 1);
    CHECK(events[0].auctionId == Id("auction-w"));
    CHECK(events[0].adSpotId == Id(uint64_t(1)));
    CHECK(events[0].bid.agent == "high");
    CHECK(events[0].lossTimeout > std::chrono::system_clock::now());

    const std::string item = bidItemId(Id("auction-w"), Id(uint64_t(1)));
    CHECK(env.banker->authorizedItems() == std::set<std::string>{"acct:low|" + item, "acct:high|" + item});
    CHECK(env.banker->cancelledItems() == std::set<std::string>{"acct:low|" + item});

    REQUIRE(waitUntil([&] { return low->losses == 1; }));
    CHECK(high->losses == 0);

    json stats = env.router.stats();
    CHECK(stats["auctions"] == 1);
    CHECK(stats["auctionsSent"] == 1);
    CHECK(stats["bids"] == 2);
    CHECK(stats["outbid"] == 1);
    CHECK(stats["agents"] == 2);
}

TEST_CASE("router: WIN LOSS and CAMPAIGN_EVENT are forwarded to the named agent")
{
    Env env;
    auto one = env.addAgent("one", "acct:one", 1.0);
    auto two = env.addAgent("two", "acct:two", 1.0);

    MatchedWinLoss win;
    win.type = MatchedWinLoss::Type::WIN;
    win.auctionId = Id("a");
    win.adSpotId = Id(uint64_t(1));
    win.winPrice = USD_CPM(1.5);
    win.bid = sampleResponse(0, 2.0, "one");
    env.router.onMatchedWinLoss(win);

    REQUIRE(waitUntil([&] { return one->wins == 1; }));
    CHECK(one->lastWinPrice == USD_CPM(1.5));

    win.type = MatchedWinLoss::Type::LOSS;
    win.bid.agent = "two";
    env.router.onMatchedWinLoss(win);
    REQUIRE(waitUntil([&] { return two->losses == 1; }));

    MatchedCampaignEvent click;
    click.event = CampaignEventType::CLICK;
    click.auctionId = Id("a");
    click.bid = sampleResponse(0, 2.0, "two");
    env.router.onMatchedCampaignEvent(click);
    REQUIRE(waitUntil([&] { return two->campaignEvents == 1; }));

    // Unknown agents are ignored.
    click.bid.agent = "nobody";
    env.router.onMatchedCampaignEvent(click);

    CHECK(one->losses == 0);
    CHECK(two->wins == 0);
    CHECK(one->campaignEvents == 0);
    json stats = env.router.stats();
    CHECK(stats["wins"] == 1);
    CHECK(stats["losses"] == 1);
    CHECK(stats["campaignEvents"] == 1);
}

TEST_CASE("router: banker refusal gives NOBUDGET and drops the bid")
{
    Env env;
    auto poor = env.addAgent("poor", "poor:acct", 5.0);
    auto rich = env.addAgent("rich", "rich:acct", 1.0);

    auto run = env.submit("auction-nb");
    REQUIRE(run.waitDone());

    REQUIRE(waitUntil([&] { return poor->nobudget == 1; }));
    CHECK(rich->nobudget == 0);

    // The refused bid, although higher, is not in the auction.
    auto best = run.auction->bestResponse(0);
    REQUIRE(best);
    CHECK(best->agent == "rich");
    CHECK(run.auction->numResponses(0) == 1);
    CHECK(env.router.stats()["nobudget"] == 1);

    // Only the rich agent can lose or win; nothing to cancel for the poor one.
    CHECK(env.banker->cancelledItems().empty());
}

TEST_CASE("router: a bid after the deadline gets TOOLATE")
{
    Env env;
    auto agent = env.addAgent("slow", "acct:slow", 1.0);
    agent->silent = true;

    auto run = env.submit("auction-late", 80);
    REQUIRE(run.waitDone());
    CHECK(std::chrono::steady_clock::now() - run.started >= 70ms);
    CHECK_FALSE(run.auction->hasResponses());

    REQUIRE(waitUntil([&] { return agent->auctions == 1; }));
    Bids bids;
    Bid bid;
    bid.spotIndex = 0;
    bid.creativeIndex = 0;
    bid.price = USD_CPM(1.0);
    bids.push_back(bid);
    agent->agent.doBid(agent->heardAuctions().front(), bids);

    REQUIRE(waitUntil([&] { return agent->toolate == 1; }));
    CHECK(env.router.stats()["toolate"] == 1);
    CHECK(env.submittedEvents().empty());
}

TEST_CASE("router: expiry finishes an auction nobody answers")
{
    Env env;
    auto agent = env.addAgent("mute", "acct:mute", 1.0);
    agent->silent = true;

    auto run = env.submit("auction-exp", 100);
    REQUIRE(run.waitDone());
    const auto elapsed = std::chrono::steady_clock::now() - run.started;
    CHECK(elapsed >= 90ms);
    CHECK(elapsed < 1000ms);
    CHECK_FALSE(run.auction->hasResponses());
    CHECK(env.submittedEvents().empty());
    CHECK(env.router.stats()["expired"] == 1);
    CHECK(env.router.stats()["auctionsInFlight"] == 0);
}

TEST_CASE("router: an agent that passes lets the auction finish early")
{
    Env env;
    auto agent = env.addAgent("pass", "acct:pass", 0);   // empty bid list

    auto run = env.submit("auction-pass", 1000);
    REQUIRE(run.waitDone());
    CHECK(std::chrono::steady_clock::now() - run.started < 500ms);
    CHECK_FALSE(run.auction->hasResponses());
    CHECK(env.router.stats()["expired"] == 0);
}

TEST_CASE("router: bids that do not fit the auction are INVALID")
{
    Env env;
    auto agent = env.addAgent("bad", "acct:bad", 1.0);

    SECTION("creative that is not eligible")
    {
        agent->creative = 5;
    }
    SECTION("price of zero")
    {
        agent->cpm = 0.0001;   // rounds to a zero price
    }

    auto run = env.submit("auction-inv");
    REQUIRE(run.waitDone());
    REQUIRE(waitUntil([&] { return agent->invalid == 1; }));
    CHECK_FALSE(agent->reason().empty());
    CHECK_FALSE(run.auction->hasResponses());
    CHECK(env.router.stats()["invalid"] == 1);
    CHECK(env.banker->authorizedItems().empty());
}

TEST_CASE("router: no matching agent finishes the auction at once")
{
    Env env;
    // 728x90 creative: no spot of the sample request accepts it.
    AgentConfig config = AgentConfig::parse(
        R"({"account": "acct:wide", "creatives": [{"id": 0, "format": "728x90"}]})");
    TestAgent agent("wide", env.router.boundEndpoint(), 1.0, config);
    REQUIRE(agent.agent.waitForConfig(2000ms));

    auto run = env.submit("auction-nofit", 2000);
    REQUIRE(run.waitDone(500ms));
    CHECK(agent.auctions == 0);
    json stats = env.router.stats();
    CHECK(stats["noAgents"] == 1);
    CHECK(stats["auctionsSent"] == 0);
}

TEST_CASE("router: bidProbability 0 never sends auctions")
{
    Env env;
    AgentConfig config = sampleAgentConfig("shy", "acct:shy");
    config.bidProbability = 0.0f;
    TestAgent agent("shy", env.router.boundEndpoint(), 1.0, config);
    REQUIRE(agent.agent.waitForConfig(2000ms));

    auto run = env.submit("auction-shy", 2000);
    REQUIRE(run.waitDone(500ms));
    CHECK(agent.auctions == 0);
}

TEST_CASE("router: a silent agent is dropped, a ponging agent stays")
{
    Env env;
    auto alive = env.addAgent("alive", "acct:alive", 1.0);
    auto gone = env.addAgent("gone", "acct:gone", 1.0);
    REQUIRE(env.router.stats()["agents"] == 2);

    gone->agent.stop();   // closes the socket: no more PONGs

    REQUIRE(waitUntil([&] { return env.router.stats()["agents"] == 1; }, 3000ms));
    std::this_thread::sleep_for(600ms);   // longer than the 400 ms timeout
    CHECK(env.router.stats()["agents"] == 1);

    auto run = env.submit("auction-after", 500);
    REQUIRE(run.waitDone());
    CHECK(alive->auctions == 1);
    CHECK(gone->auctions == 0);
    auto best = run.auction->bestResponse(0);
    REQUIRE(best);
    CHECK(best->agent == "alive");
}

TEST_CASE("router: disconnect while an auction is open does not block it")
{
    Env env;
    auto stays = env.addAgent("stays", "acct:stays", 1.0);
    auto leaves = env.addAgent("leaves", "acct:leaves", 1.0);
    leaves->silent = true;

    auto run = env.submit("auction-leave", 5000);
    REQUIRE(waitUntil([&] { return leaves->auctions == 1; }));
    leaves->agent.stop();

    // "stays" answered; the router finishes once "leaves" times out, long
    // before the 5 second expiry.
    REQUIRE(run.waitDone(3000ms));
    auto best = run.auction->bestResponse(0);
    REQUIRE(best);
    CHECK(best->agent == "stays");
}

TEST_CASE("router: protocol errors get ERROR and unknown agents get NEEDCONFIG")
{
    Env env;
    zmq::context_t context;
    zmq::socket_t dealer(context, zmq::socket_type::dealer);
    dealer.set(zmq::sockopt::routing_id, "raw");
    dealer.set(zmq::sockopt::linger, 0);
    dealer.set(zmq::sockopt::rcvtimeo, 2000);
    dealer.connect(env.router.boundEndpoint());

    auto exchange = [&](const protocol::Frames& frames) {
        zmqutil::sendFrames(dealer, frames);
        std::vector<std::string> reply;
        REQUIRE(zmqutil::recvFramesWait(dealer, reply, 2000ms));
        return reply;
    };

    SECTION("bad json in CONFIG")
    {
        auto reply = exchange({"CONFIG", "raw", "{not json"});
        REQUIRE(reply.size() >= 2);
        CHECK(reply[0] == "ERROR");
    }
    SECTION("config the AgentConfig parser rejects")
    {
        auto reply = exchange({"CONFIG", "raw", R"({"creatives": []})"});
        REQUIRE_FALSE(reply.empty());
        CHECK(reply[0] == "ERROR");
    }
    SECTION("message before CONFIG")
    {
        auto reply = exchange({"PING", "token"});
        REQUIRE(reply.size() == 1);
        CHECK(reply[0] == "NEEDCONFIG");
    }
    SECTION("PING after CONFIG")
    {
        auto reply = exchange(protocol::encode(
            protocol::ConfigMsg{"raw", sampleAgentConfig("raw", "acct:raw")}));
        REQUIRE(reply.size() == 2);
        CHECK(reply[0] == "GOTCONFIG");
        reply = exchange({"PING", "token"});
        REQUIRE(reply.size() == 2);
        CHECK(reply[0] == "PONG");
        CHECK(reply[1] == "token");
    }
}

TEST_CASE("router: stop finishes the auctions in flight")
{
    auto env = std::make_unique<Env>();
    auto agent = env->addAgent("mute", "acct:mute", 1.0);
    agent->silent = true;

    auto run = env->submit("auction-stop", 10000);
    REQUIRE(waitUntil([&] { return agent->auctions == 1; }));
    env->router.stop();
    REQUIRE(run.waitDone(500ms));

    // After stop, new auctions are finished without bids right away.
    auto late = env->submit("auction-after-stop", 10000);
    REQUIRE(late.waitDone(500ms));
    env->router.stop();   // twice is fine
}

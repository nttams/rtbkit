// The whole stack in one process on random ports: HTTP client (the exchange)
// -> OpenRTB connector -> router -> BiddingAgents over zmq -> banker; then the
// win notice (nurl) -> ad server -> post auction -> banker and back to the
// agents. Short timeouts keep it fast.

#include "agent/bidding_agent.h"
#include "exchange/mock_exchange_support.h"
#include "server/core_server.h"

#include <httplib.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

using namespace RTBKIT;
using nlohmann::json;
using namespace std::chrono_literals;

namespace {

template<typename Predicate>
bool waitUntil(Predicate predicate, std::chrono::milliseconds limit = 3000ms)
{
    const auto end = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < end) {
        if (predicate()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return predicate();
}

AgentConfig agentConfig(const std::string& name, const std::string& account)
{
    AgentConfig config = AgentConfig::fromJson(json{
        {"account", account},
        {"creatives", json::array({json{
            {"id", 1}, {"format", "300x250"},
            {"providerConfig", {{"openrtb", {{"crid", "crid-" + name},
                                             {"adm", "<b>" + name + "</b>"},
                                             {"adomain", json::array({name + ".example.com"})}}}}}}})},
        {"exchangeFilter", {{"include", json::array({"openrtb"})}}}});
    config.name = name;
    return config;
}

/** Fixed price bidder that counts what the router tells it. */
struct Bidder {
    Bidder(const std::string& name, const std::string& account, double cpm,
           const std::string& endpoint)
        : agent(name, agentConfig(name, account), endpoint)
    {
        const Amount price = USD_CPM(cpm);
        agent.onBidRequest = [this, price](const Id& id, std::shared_ptr<const BidRequest>,
                                           const std::vector<protocol::AuctionSpot>& spots,
                                           double) {
            ++auctions;
            Bids bids;
            for (const auto& spot : spots) {
                if (spot.creatives.empty()) continue;
                Bid bid;
                bid.spotIndex = spot.spotIndex;
                bid.creativeIndex = spot.creatives.front();
                bid.price = price;
                bids.push_back(bid);
            }
            agent.doBid(id, bids);
        };
        agent.onWin = [this](const MatchedWinLoss& e) {
            {
                std::lock_guard lock(mutex);
                winPrices.push_back(e.winPrice);
            }
            ++wins;
        };
        agent.onLoss = [this](const MatchedWinLoss&) { ++losses; };
        agent.onNoBudget = [this](const Id&) { ++nobudget; };
        agent.onCampaignEvent = [this](const MatchedCampaignEvent&) { ++campaignEvents; };
        agent.start();
        REQUIRE(agent.waitForConfig(3000ms));
    }

    Amount totalWon()
    {
        std::lock_guard lock(mutex);
        Amount sum;
        for (const auto& p : winPrices) sum += p;
        return sum;
    }

    std::atomic<int> auctions{0}, wins{0}, losses{0}, nobudget{0}, campaignEvents{0};
    std::mutex mutex;
    std::vector<Amount> winPrices;
    BiddingAgent agent;
};

json serverConfig(const json& accounts, int lossTimeoutMs)
{
    return json{{"router", {{"agentEndpoint", "tcp://127.0.0.1:*"},
                            {"lossTimeoutMs", lossTimeoutMs}}},
                {"exchange", {{"port", 0}, {"threads", 8}}},
                {"adserver", {{"port", 0}, {"threads", 4}}},
                {"postAuction", {{"tickIntervalMs", 20}}},
                {"banker", {{"type", "memory"}, {"accounts", accounts}}}};
}

struct BidResult {
    int status = 0;
    json bid;   // first bid of the response, if any
};

struct Stack {
    explicit Stack(const json& accounts, int lossTimeoutMs = 300)
        : server(serverConfig(accounts, lossTimeoutMs))
    {
        server.start();
    }

    BidResult request()
    {
        const json req = {
            {"id", "it-" + std::to_string(++counter)},
            {"at", 2}, {"tmax", 300}, {"cur", json::array({"USD"})},
            {"imp", json::array({json{{"id", "1"},
                                      {"banner", {{"w", 300}, {"h", 250}}}}})},
            {"site", {{"id", "s"}, {"domain", "example.com"}, {"page", "http://example.com/"}}},
            {"device", {{"ip", "10.1.2.3"}, {"ua", "test"}, {"language", "en"}}},
            {"user", {{"id", "user-1"}}}};
        httplib::Client client("127.0.0.1", server.exchangePort());
        client.set_read_timeout(5);
        auto res = client.Post("/auctions", req.dump(), "application/json");
        REQUIRE(res);
        BidResult result;
        result.status = res->status;
        if (res->status == 200) {
            json body = json::parse(res->body);
            result.bid = body["seatbid"][0]["bid"][0];
        }
        return result;
    }

    /** The exchange calls the nurl with the clearing price (CPM). */
    void win(const json& bid, double clearingCpm)
    {
        const std::string url =
            replaceAuctionPrice(bid["nurl"].get<std::string>(), clearingCpm);
        auto [origin, path] = splitUrl(url);
        httplib::Client client(origin);
        auto res = client.Get(path);
        REQUIRE(res);
        CHECK(res->status == 200);
    }

    Amount spent(const std::string& account)
    {
        return server.banker().getAccountSummary(AccountKey(account)).spent;
    }
    Amount inFlight(const std::string& account)
    {
        return server.banker().getAccountSummary(AccountKey(account)).inFlight;
    }

    CoreServer server;
    int counter = 0;
};

const json richAccounts = {{"t:rich", {{"budget", "100000000USD/1M"}}},
                           {"t:other", {{"budget", "100000000USD/1M"}}}};

} // namespace

TEST_CASE("integration: winner is charged the clearing price, loser is told LOSS")
{
    Stack stack(richAccounts);
    Bidder high("high", "t:rich", 3.0, stack.server.agentEndpoint());
    Bidder low("low", "t:other", 1.5, stack.server.agentEndpoint());

    auto result = stack.request();
    REQUIRE(result.status == 200);
    CHECK(result.bid["cid"] == "high");
    CHECK(result.bid["crid"] == "crid-high");
    CHECK(result.bid["adm"] == "<b>high</b>");
    CHECK(result.bid["price"].get<double>() == 3.0);   // CPM on the wire
    CHECK(high.auctions == 1);
    CHECK(low.auctions == 1);

    // The reservation is held until the win notice (or the loss timeout).
    CHECK(stack.inFlight("t:rich") == USD_CPM(3.0));
    CHECK(stack.spent("t:rich").isZero());

    stack.win(result.bid, 2.0);   // second price: the exchange clears below the bid
    REQUIRE(waitUntil([&] { return high.wins == 1; }));
    CHECK(high.totalWon() == USD_CPM(2.0));
    CHECK(stack.spent("t:rich") == USD_CPM(2.0));
    CHECK(stack.inFlight("t:rich").isZero());

    // The loser was outbid: told at once, reservation released, nothing spent.
    REQUIRE(waitUntil([&] { return low.losses == 1; }));
    CHECK(stack.spent("t:other").isZero());
    CHECK(stack.inFlight("t:other").isZero());
    CHECK(high.losses == 0);
    CHECK(low.wins == 0);

    // Impression and click reach the winner.
    const std::string impression = "/impression?auctionId="
        + urlEncode(queryParam(result.bid["nurl"].get<std::string>(), "auctionId"))
        + "&adSpotId=1";
    httplib::Client client("127.0.0.1", stack.server.adServerPort());
    REQUIRE(client.Get(impression));
    REQUIRE(waitUntil([&] { return high.campaignEvents == 1; }));
}

TEST_CASE("integration: banker spent equals the sum of the win prices")
{
    Stack stack(richAccounts);
    Bidder high("high", "t:rich", 3.0, stack.server.agentEndpoint());
    Bidder low("low", "t:other", 1.0, stack.server.agentEndpoint());

    const double prices[] = {3.0, 2.5, 1.25, 0.75, 2.0, 3.0};
    Amount expected;
    for (double price : prices) {
        auto result = stack.request();
        REQUIRE(result.status == 200);
        stack.win(result.bid, price);
        expected += USD_CPM(price);
    }
    REQUIRE(waitUntil([&] { return high.wins == 6; }));
    CHECK(high.totalWon() == expected);
    CHECK(stack.spent("t:rich") == expected);
    CHECK(stack.spent("t:other").isZero());
    // The parent account sums its children.
    CHECK(stack.spent("t") == expected);
    CHECK(stack.inFlight("t:rich").isZero());
    REQUIRE(waitUntil([&] { return low.losses == 6; }));
    CHECK(stack.inFlight("t:other").isZero());

    json stats = stack.server.statsJson();
    CHECK(stats["router"]["wins"] == 6);
    CHECK(stats["postAuction"]["wins"] == 6);
    CHECK(stats["connector"]["bids"] == 6);
}

TEST_CASE("integration: no win notice means LOSS after the timeout and nothing is spent")
{
    Stack stack(richAccounts, 200);
    Bidder only("only", "t:rich", 2.0, stack.server.agentEndpoint());

    auto result = stack.request();
    REQUIRE(result.status == 200);
    CHECK(stack.inFlight("t:rich") == USD_CPM(2.0));

    REQUIRE(waitUntil([&] { return only.losses == 1; }, 3000ms));
    CHECK(only.wins == 0);
    CHECK(stack.inFlight("t:rich").isZero());
    CHECK(stack.spent("t:rich").isZero());
    CHECK(stack.server.postAuction().numSubmitted() == 0);

    // A win notice that comes after the timeout is still charged (late win).
    stack.win(result.bid, 1.5);
    REQUIRE(waitUntil([&] { return only.wins == 1; }));
    CHECK(stack.spent("t:rich") == USD_CPM(1.5));
    CHECK(stack.server.statsJson()["postAuction"]["lateWins"] == 1);
}

TEST_CASE("integration: NOBUDGET when the budget is used up, and a settled bid frees room")
{
    // 4000 micro USD: two 1.5 CPM bids (1500 micro USD each) fit, a third does not.
    Stack stack(json{{"t:poor", {{"budget", "4000USD/1M"}}}}, 5000);
    Bidder poor("poor", "t:poor", 1.5, stack.server.agentEndpoint());

    auto first = stack.request();
    auto second = stack.request();
    REQUIRE(first.status == 200);
    REQUIRE(second.status == 200);
    CHECK(stack.inFlight("t:poor") == USD_CPM(3.0));

    auto third = stack.request();
    CHECK(third.status == 204);
    REQUIRE(waitUntil([&] { return poor.nobudget == 1; }));
    CHECK(stack.server.router().stats()["nobudget"] == 1);
    CHECK(stack.inFlight("t:poor") == USD_CPM(3.0));

    // Winning the first bid at 1.0 CPM spends 1000 and releases its 1500:
    // 4000 - 1000 - 1500 leaves exactly room for one more bid.
    stack.win(first.bid, 1.0);
    REQUIRE(waitUntil([&] { return poor.wins == 1; }));
    CHECK(stack.spent("t:poor") == USD_CPM(1.0));
    CHECK(stack.inFlight("t:poor") == USD_CPM(1.5));

    auto fourth = stack.request();
    CHECK(fourth.status == 200);
    CHECK(stack.inFlight("t:poor") == USD_CPM(3.0));
}

TEST_CASE("integration: agents sharing an account are reserved separately")
{
    // Enough for two reservations only if they are separate items: 3000 + 1500.
    Stack stack(json{{"t:shared", {{"budget", "4500USD/1M"}}}}, 5000);
    Bidder high("high", "t:shared", 3.0, stack.server.agentEndpoint());
    Bidder low("low", "t:shared", 1.5, stack.server.agentEndpoint());

    auto result = stack.request();
    REQUIRE(result.status == 200);
    CHECK(result.bid["cid"] == "high");
    CHECK(low.nobudget == 0);
    // The loser's reservation is released at the end of the auction; the
    // winner's one stays until its win notice.
    REQUIRE(waitUntil([&] { return low.losses == 1; }));
    CHECK(stack.inFlight("t:shared") == USD_CPM(3.0));

    stack.win(result.bid, 2.0);
    REQUIRE(waitUntil([&] { return high.wins == 1; }));
    CHECK(stack.spent("t:shared") == USD_CPM(2.0));
    CHECK(stack.inFlight("t:shared").isZero());
}

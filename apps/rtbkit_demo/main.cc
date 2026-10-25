// All-in-one demo: the whole RTBKit stack in one process.
//
//   rtbkit_demo --requests 2000 --qps 500     generate load, print a summary, exit
//   rtbkit_demo --serve                       keep the services up until SIGINT
//
// Runs the core server (banker, router, OpenRTB connector on 12350, post
// auction, ad server on 12360, see config/rtbkit.json) and two BiddingAgents
// (config/agents/agent_a.json at --cpm-a, agent_b.json at --cpm-b). Agent B
// has a small budget, so it hits NOBUDGET. The load generator is the mock
// exchange logic (generateOpenRtbRequest) running on threads in this process:
// it posts requests, then calls the win notice (nurl) of a share of the bids.
//
// Options: --config FILE  --agent-a FILE  --agent-b FILE  --cpm-a X  --cpm-b X
//          --requests N  --qps Q  --win-rate R  --click-rate R  --threads N
//          --seed N  --serve
// Exit code: 0 if there were bids, wins and banker spend, else 1.

#include "agent/bidding_agent.h"
#include "exchange/mock_exchange_support.h"
#include "server/core_server.h"

#include <httplib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace RTBKIT;
using nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {

std::atomic<bool> stopRequested{false};
void onSignal(int) { stopRequested = true; }

struct Options {
    std::string configFile = std::string(RTB_CONFIG_DIR) + "/rtbkit.json";
    std::string agentA = std::string(RTB_CONFIG_DIR) + "/agents/agent_a.json";
    std::string agentB = std::string(RTB_CONFIG_DIR) + "/agents/agent_b.json";
    double cpmA = 2.0;
    double cpmB = 1.2;
    int64_t requests = -1;
    double qps = 200;
    double winRate = 0.3;
    double clickRate = 0.1;
    int threads = 16;
    uint64_t seed = 0;
    bool serve = false;
};

Options parseArgs(int argc, char** argv)
{
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) throw std::invalid_argument("missing value for " + arg);
            return argv[++i];
        };
        if (arg == "--config") o.configFile = value();
        else if (arg == "--agent-a") o.agentA = value();
        else if (arg == "--agent-b") o.agentB = value();
        else if (arg == "--cpm-a") o.cpmA = std::stod(value());
        else if (arg == "--cpm-b") o.cpmB = std::stod(value());
        else if (arg == "--requests") o.requests = std::stoll(value());
        else if (arg == "--qps") o.qps = std::stod(value());
        else if (arg == "--win-rate") o.winRate = std::stod(value());
        else if (arg == "--click-rate") o.clickRate = std::stod(value());
        else if (arg == "--threads") o.threads = std::stoi(value());
        else if (arg == "--seed") o.seed = std::stoull(value());
        else if (arg == "--serve") o.serve = true;
        else throw std::invalid_argument(
            "unknown option " + arg + "\nsee the header of apps/rtbkit_demo/main.cc");
    }
    if (o.qps <= 0 || o.threads < 1 || o.cpmA <= 0 || o.cpmB <= 0 || o.winRate < 0
        || o.winRate > 1 || o.clickRate < 0 || o.clickRate > 1)
        throw std::invalid_argument("option value out of range");
    if (o.requests < 0 && !o.serve) o.requests = 1000;
    if (o.serve) o.requests = -1;
    if (o.seed == 0) o.seed = std::random_device{}();
    return o;
}

json readJsonFile(const std::string& path)
{
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot read " + path);
    return json::parse(in);
}

/** One in-process bidding agent that bids a fixed CPM on the first eligible
    creative of every spot. */
struct DemoAgent {
    DemoAgent(const std::string& name, const std::string& configFile, double cpm,
              const std::string& endpoint)
        : name(name), price(USD_CPM(cpm)),
          agent(name, makeConfig(name, configFile), endpoint)
    {
        agent.onBidRequest = [this](const Id& auctionId,
                                    std::shared_ptr<const BidRequest> request,
                                    const std::vector<protocol::AuctionSpot>& spots, double) {
            ++auctions;
            Bids bids;
            for (const auto& spot : spots) {
                if (spot.creatives.empty()) continue;
                if (request->imp.at(size_t(spot.spotIndex)).reservePrice > price) continue;
                Bid bid;
                bid.spotIndex = spot.spotIndex;
                bid.creativeIndex = spot.creatives.front();
                bid.price = price;
                bids.push_back(bid);
            }
            (bids.empty() ? passes : bidsSent) += 1;
            agent.doBid(auctionId, bids);
        };
        agent.onWin = [this](const MatchedWinLoss&) { ++wins; };
        agent.onLoss = [this](const MatchedWinLoss&) { ++losses; };
        agent.onNoBudget = [this](const Id&) { ++nobudget; };
        agent.onTooLate = [this](const Id&) { ++tooLate; };
        agent.onInvalid = [this](const Id&, const std::string&) { ++invalid; };
        agent.onCampaignEvent = [this](const MatchedCampaignEvent& e) {
            (e.event == CampaignEventType::CLICK ? clicks : impressions) += 1;
        };
    }

    static AgentConfig makeConfig(const std::string& name, const std::string& file)
    {
        AgentConfig config = AgentConfig::fromJson(readJsonFile(file));
        config.name = name;
        return config;
    }

    json counters() const
    {
        return json{{"auctions", auctions.load()}, {"bids", bidsSent.load()},
                    {"passes", passes.load()}, {"wins", wins.load()},
                    {"losses", losses.load()}, {"nobudget", nobudget.load()},
                    {"toolate", tooLate.load()}, {"invalid", invalid.load()},
                    {"impressions", impressions.load()}, {"clicks", clicks.load()}};
    }

    std::string name;
    Amount price;
    std::atomic<uint64_t> auctions{0}, bidsSent{0}, passes{0}, wins{0}, losses{0},
        nobudget{0}, tooLate{0}, invalid{0}, impressions{0}, clicks{0};
    BiddingAgent agent;
};

struct LoadTotals {
    uint64_t sent = 0, bidResponses = 0, noBids = 0, errors = 0, winsNotified = 0,
             winErrors = 0, impressions = 0, clicks = 0;
    int64_t paidMicros = 0;   // sum of the win prices we called, per impression
    std::vector<double> latenciesMs;

    void add(const LoadTotals& o)
    {
        sent += o.sent; bidResponses += o.bidResponses; noBids += o.noBids;
        errors += o.errors; winsNotified += o.winsNotified; winErrors += o.winErrors;
        impressions += o.impressions; clicks += o.clicks; paidMicros += o.paidMicros;
        latenciesMs.insert(latenciesMs.end(), o.latenciesMs.begin(), o.latenciesMs.end());
    }
};

std::unique_ptr<httplib::Client> makeClient(const std::string& origin)
{
    auto client = std::make_unique<httplib::Client>(origin);
    client->set_connection_timeout(2);
    client->set_read_timeout(3);
    client->set_keep_alive(true);
    return client;
}

bool getOk(std::map<std::string, std::unique_ptr<httplib::Client>>& clients,
           const std::string& url)
{
    try {
        auto [origin, path] = splitUrl(url);
        auto& client = clients[origin];
        if (!client) client = makeClient(origin);
        auto res = client->Get(path);
        return res && res->status == 200;
    }
    catch (const std::invalid_argument&) {
        return false;
    }
}

/** What the mock exchange does: post requests at --qps, call the nurl of a
    share of the bids with the clearing price in CPM. */
void loadWorker(int index, const Options& o, int exchangePort, int adServerPort,
                std::atomic<int64_t>& next, Clock::time_point start, LoadTotals& t)
{
    std::mt19937_64 rng(o.seed + uint64_t(index) * 7919);
    std::uniform_real_distribution<double> unit(0, 1);
    auto exchange = makeClient("http://127.0.0.1:" + std::to_string(exchangePort));
    std::map<std::string, std::unique_ptr<httplib::Client>> others;
    const std::string adserver = "http://127.0.0.1:" + std::to_string(adServerPort);

    while (!stopRequested) {
        const int64_t n = next.fetch_add(1);
        if (n >= o.requests) break;
        std::this_thread::sleep_until(
            start + std::chrono::duration_cast<Clock::duration>(
                        std::chrono::duration<double>(double(n) / o.qps)));

        const json request = generateOpenRtbRequest(rng);
        const auto sentAt = Clock::now();
        auto res = exchange->Post("/auctions", request.dump(), "application/json");
        t.latenciesMs.push_back(
            std::chrono::duration<double, std::milli>(Clock::now() - sentAt).count());
        ++t.sent;

        if (!res) { ++t.errors; continue; }
        if (res->status == 204) { ++t.noBids; continue; }
        if (res->status != 200) { ++t.errors; continue; }
        const json response = json::parse(res->body, nullptr, false);
        if (!response.is_object() || !response.contains("seatbid")) { ++t.errors; continue; }
        ++t.bidResponses;

        for (const auto& seat : response["seatbid"]) {
            for (const auto& bid : seat.value("bid", json::array())) {
                if (unit(rng) >= o.winRate) continue;
                // Second price auctions clear below the bid: half the time pay
                // 50-100% of it. The exchange's clearing price is what counts.
                double price = bid["price"].get<double>();
                if (unit(rng) < 0.5) price *= 0.5 + 0.5 * unit(rng);
                const std::string nurl =
                    replaceAuctionPrice(bid["nurl"].get<std::string>(), price);
                if (!getOk(others, nurl)) { ++t.winErrors; continue; }
                ++t.winsNotified;
                t.paidMicros += Amount::fromCpm(CurrencyCode::USD,
                                                std::stod(queryParam(nurl, "price"))).value;

                const std::string query = "?auctionId=" + urlEncode(queryParam(nurl, "auctionId"))
                    + "&adSpotId=" + urlEncode(queryParam(nurl, "adSpotId"));
                if (getOk(others, adserver + "/impression" + query)) ++t.impressions;
                if (unit(rng) < o.clickRate && getOk(others, adserver + "/click" + query))
                    ++t.clicks;
            }
        }
    }
}

std::string usd(const Amount& amount)
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.6f USD", amount.toUnits());
    return buf;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const Options o = parseArgs(argc, argv);

        json config = readJsonFile(o.configFile);
        // The demo does not clash with a running rtbkit_router: agents are
        // in-process on a random zmq port, no admin port, short loss timeout.
        config.erase("admin");
        config["router"]["agentEndpoint"] = "tcp://127.0.0.1:*";
        config["router"]["lossTimeoutMs"] = 1000;

        CoreServer server(config);
        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);
        server.start();

        DemoAgent a("agent_a", o.agentA, o.cpmA, server.agentEndpoint());
        DemoAgent b("agent_b", o.agentB, o.cpmB, server.agentEndpoint());
        a.agent.start();
        b.agent.start();
        if (!a.agent.waitForConfig(std::chrono::seconds(3))
            || !b.agent.waitForConfig(std::chrono::seconds(3)))
            throw std::runtime_error("agents did not get their config accepted");

        std::cout << "rtbkit_demo: exchange http://127.0.0.1:" << server.exchangePort()
                  << "/auctions, adserver http://127.0.0.1:" << server.adServerPort()
                  << ", agents " << server.agentEndpoint() << std::endl;

        LoadTotals load;
        double elapsed = 0;
        if (o.requests >= 0) {
            std::cout << "load: " << o.requests << " requests at " << o.qps << " qps, win rate "
                      << o.winRate << ", seed " << o.seed << std::endl;
            std::atomic<int64_t> next{0};
            const auto start = Clock::now() + std::chrono::milliseconds(50);
            std::vector<LoadTotals> totals(size_t(o.threads));
            std::vector<std::thread> threads;
            for (int i = 0; i < o.threads; ++i)
                threads.emplace_back(loadWorker, i, std::cref(o), server.exchangePort(),
                                     server.adServerPort(), std::ref(next), start,
                                     std::ref(totals[size_t(i)]));
            for (auto& t : threads) t.join();
            elapsed = std::chrono::duration<double>(Clock::now() - start).count();
            for (const auto& t : totals) load.add(t);

            // Let the unclaimed bids time out (loss) so that nothing stays in flight.
            const auto limit = Clock::now() + std::chrono::seconds(5);
            while (Clock::now() < limit && server.postAuction().numSubmitted() > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        else {
            std::cout << "serving until SIGINT" << std::endl;
            while (!stopRequested)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        const json stats = server.statsJson();
        Amount spent;
        std::cout << "\n== load generator ==\n";
        if (o.requests >= 0) {
            std::printf("sent %llu (%.1f req/s over %.2f s), bid responses %llu, no bids %llu, "
                        "errors %llu\n",
                        (unsigned long long)load.sent,
                        elapsed > 0 ? double(load.sent) / elapsed : 0.0, elapsed,
                        (unsigned long long)load.bidResponses, (unsigned long long)load.noBids,
                        (unsigned long long)load.errors);
            std::printf("latency p50 %.2f ms, p99 %.2f ms\n",
                        percentile(load.latenciesMs, 0.5), percentile(load.latenciesMs, 0.99));
            std::printf("win notices %llu (failed %llu), impressions %llu, clicks %llu, "
                        "win prices called %.6f USD\n",
                        (unsigned long long)load.winsNotified, (unsigned long long)load.winErrors,
                        (unsigned long long)load.impressions, (unsigned long long)load.clicks,
                        double(load.paidMicros) / 1e6);
        }
        std::cout << "== router ==\n" << stats["router"].dump() << "\n"
                  << "== connector ==\n" << stats["connector"].dump() << "\n"
                  << "== post auction ==\n" << stats["postAuction"].dump() << "\n"
                  << "== banker accounts ==\n";
        for (const AccountKey& account : server.banker().accounts()) {
            const AccountSummary s = server.banker().getAccountSummary(account);
            std::printf("%-14s budget %-18s spent %-18s inFlight %-18s available %s\n",
                        account.toString().c_str(), usd(s.budget).c_str(),
                        usd(s.spent).c_str(), usd(s.inFlight).c_str(), usd(s.available()).c_str());
            if (account.toString() == "acme") spent = s.spent;
        }
        std::cout << "== agents ==\nagent_a (" << o.cpmA << " CPM): " << a.counters().dump()
                  << "\nagent_b (" << o.cpmB << " CPM): " << b.counters().dump() << std::endl;

        a.agent.stop();
        b.agent.stop();
        server.stop();

        if (o.requests < 0) return 0;
        const bool ok = stats["connector"]["bids"].get<uint64_t>() > 0
            && stats["router"]["wins"].get<uint64_t>() > 0 && spent.value > 0;
        std::cout << (ok ? "\nDEMO OK" : "\nDEMO FAILED: no bids, wins or banker spend") << std::endl;
        return ok ? 0 : 1;
    }
    catch (const std::exception& e) {
        std::cerr << "rtbkit_demo: " << e.what() << std::endl;
        return 1;
    }
}

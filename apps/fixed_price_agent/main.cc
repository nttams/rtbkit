// Bidding agent that bids a fixed CPM on the first eligible creative of every
// spot it is offered.
//
//   fixed_price_agent -c agent.json [-r tcp://127.0.0.1:12340] [-p 1.0] [-n name]
//
// -c  AgentConfig JSON file (required)
// -r  router agent endpoint (default tcp://127.0.0.1:12340)
// -p  CPM to bid, in USD (default 1.0)
// -n  agent name, also the zmq identity (default fixed_price_agent)
// Prints its counters every second; stops cleanly on SIGINT / SIGTERM.

#include "agent/bidding_agent.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

using namespace RTBKIT;

namespace {

volatile std::sig_atomic_t g_stop = 0;

void onSignal(int) { g_stop = 1; }

void usage()
{
    std::cerr << "usage: fixed_price_agent -c agent.json [-r endpoint] [-p cpm] [-n name]\n";
}

struct Counters {
    std::atomic<uint64_t> auctions{0}, bids{0}, passes{0}, wins{0}, losses{0},
        nobudget{0}, tooLate{0}, invalid{0}, errors{0};
};

} // namespace

int main(int argc, char** argv)
{
    std::string configPath;
    std::string endpoint = "tcp://127.0.0.1:12340";
    std::string name = "fixed_price_agent";
    double cpm = 1.0;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (i + 1 >= argc || arg.size() != 2 || arg[0] != '-') {
            usage();
            return 2;
        }
        const std::string value = argv[++i];
        switch (arg[1]) {
        case 'c': configPath = value; break;
        case 'r': endpoint = value; break;
        case 'n': name = value; break;
        case 'p': cpm = std::atof(value.c_str()); break;
        default: usage(); return 2;
        }
    }
    if (configPath.empty() || cpm <= 0) {
        usage();
        return 2;
    }

    AgentConfig config;
    try {
        std::ifstream in(configPath);
        if (!in) throw std::runtime_error("cannot read " + configPath);
        std::stringstream text;
        text << in.rdbuf();
        config = AgentConfig::parse(text.str());
    }
    catch (const std::exception& e) {
        std::cerr << "bad config: " << e.what() << "\n";
        return 1;
    }
    config.name = name;

    const Amount price = USD_CPM(cpm);
    Counters counters;
    BiddingAgent agent(name, config, endpoint);

    agent.onBidRequest = [&](const Id& auctionId,
                             std::shared_ptr<const BidRequest> request,
                             const std::vector<protocol::AuctionSpot>& spots, double) {
        ++counters.auctions;
        Bids bids;
        for (const auto& spot : spots) {
            if (spot.creatives.empty()) continue;
            if (request->imp.at(size_t(spot.spotIndex)).reservePrice > price) continue;
            Bid bid;
            bid.spotIndex = spot.spotIndex;
            bid.creativeIndex = spot.creatives.front();
            bid.price = price;
            bid.bidData = auctionId.toString();
            bids.push_back(bid);
        }
        (bids.empty() ? counters.passes : counters.bids) += 1;
        agent.doBid(auctionId, bids);
    };
    agent.onWin = [&](const MatchedWinLoss&) { ++counters.wins; };
    agent.onLoss = [&](const MatchedWinLoss&) { ++counters.losses; };
    agent.onNoBudget = [&](const Id&) { ++counters.nobudget; };
    agent.onTooLate = [&](const Id&) { ++counters.tooLate; };
    agent.onInvalid = [&](const Id&, const std::string&) { ++counters.invalid; };
    agent.onError = [&](const std::string& reason, const Id&) {
        ++counters.errors;
        std::cerr << "error: " << reason << "\n";
    };

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    agent.start();
    std::cout << name << ": bidding " << cpm << " CPM via " << endpoint << std::endl;

    auto nextPrint = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (std::chrono::steady_clock::now() < nextPrint) continue;
        nextPrint += std::chrono::seconds(1);
        std::cout << "auctions=" << counters.auctions << " bids=" << counters.bids
                  << " passes=" << counters.passes << " wins=" << counters.wins
                  << " losses=" << counters.losses << " nobudget=" << counters.nobudget
                  << " toolate=" << counters.tooLate << " invalid=" << counters.invalid
                  << " errors=" << counters.errors << std::endl;
    }

    agent.stop();
    std::cout << name << ": stopped" << std::endl;
    return 0;
}

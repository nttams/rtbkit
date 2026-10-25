// Client library for bidding agents, the modern version of
// rtbkit/plugins/bidding_agent/bidding_agent.h. It owns a ZeroMQ DEALER socket
// (identity = agent name) and one thread that sends the CONFIG, receives the
// router messages and calls the callbacks below on that thread.
//
//   BiddingAgent agent("agent1", config, "tcp://127.0.0.1:12340");
//   agent.onBidRequest = [&](const Id& id, auto request, const auto& spots, double) {
//       Bids bids; ...; agent.doBid(id, bids);
//   };
//   agent.start();
//
// Set the callbacks before start(). doBid() may be called from any thread.
// An AUCTION that has no onBidRequest callback, or whose callback throws, is
// answered with an empty bid so the router does not wait for it.
#pragma once

#include "agent/agent_config.h"
#include "agent/protocol.h"
#include "common/auction.h"
#include "common/bid_request.h"
#include "common/events.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace RTBKIT {

class BiddingAgent {
public:
    BiddingAgent(std::string name, AgentConfig config, std::string routerEndpoint);
    ~BiddingAgent();

    BiddingAgent(const BiddingAgent&) = delete;
    BiddingAgent& operator=(const BiddingAgent&) = delete;

    /// auction id, request, biddable spots (spot index + creative indexes), ms left.
    std::function<void(const Id&, std::shared_ptr<const BidRequest>,
                       const std::vector<protocol::AuctionSpot>&, double)> onBidRequest;
    std::function<void(const MatchedWinLoss&)> onWin;
    std::function<void(const MatchedWinLoss&)> onLoss;
    std::function<void(const MatchedCampaignEvent&)> onCampaignEvent;
    std::function<void(const Id&)> onNoBudget;
    std::function<void(const Id&)> onTooLate;
    std::function<void(const Id&, const std::string&)> onInvalid;   ///< auction id, reason
    std::function<void(const std::string&, const Id&)> onError;     ///< reason, auction id (may be empty)
    std::function<void()> onGotConfig;

    void start();   ///< connects, sends CONFIG, starts the thread
    void stop();    ///< joins the thread, safe to call twice

    /** Thread safe. Sends the bids (an empty list passes). */
    void doBid(const Id& auctionId, const Bids& bids);

    /** Thread safe. Sends a new config to the router. */
    void sendConfig(const AgentConfig& config);

    /** True once the router answered the CONFIG with GOTCONFIG. */
    bool hasConfig() const { return gotConfig_; }

    /** Blocks until GOTCONFIG arrives; false on timeout. */
    bool waitForConfig(std::chrono::milliseconds timeout);

    const std::string& name() const { return name_; }

private:
    struct Impl;
    std::string name_;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> gotConfig_{false};
    std::mutex mutex_;
    std::condition_variable configCv_;
};

} // namespace RTBKIT

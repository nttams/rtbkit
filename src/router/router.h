// Interface sketch of the router. The router owns one event loop thread; that
// thread alone touches the FilterPool, the agent table and the auctions in
// flight. Other threads (exchange HTTP threads, post auction) only enqueue
// work through the thread-safe entry points below.
//
// Per auction (see docs/ARCHITECTURE.md for the diagram):
//   1. injectAuction: FilterPool::filter -> configs and biddable spots; send
//      AUCTION to each selected agent; expiry = auction->expiry().
//   2. BID from an agent: check it against the biddable spots, banker
//      authorizeBid (NOBUDGET on refusal), auction->setResponse. Late bids
//      get TOOLATE, bad ones INVALID.
//   3. All agents answered or expiry reached: banker cancelBid for every
//      response that did not win; for each winning spot send a
//      SubmittedAuctionEvent to the sink; auction->finish().
#pragma once

#include "banker/banker.h"
#include "common/events.h"
#include "exchange/exchange_connector.h"

#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

namespace RTBKIT {

struct RouterConfig {
    /** ZeroMQ ROUTER endpoint that bidding agents connect to. */
    std::string agentEndpoint = "tcp://127.0.0.1:12340";

    /** All members optional. Throws std::runtime_error on bad types. */
    static RouterConfig fromJson(const nlohmann::json& j);
    nlohmann::json toJson() const;
};

class Router {
public:
    using SubmittedAuctionSink = std::function<void(const SubmittedAuctionEvent&)>;

    Router(RouterConfig config, std::shared_ptr<Banker> banker);
    ~Router();

    /** Registers a connector: sets its auction handler to injectAuction and
        uses it for prepareConfig() and as the filters' exchange hooks. */
    void addExchange(std::shared_ptr<ExchangeConnector> connector);

    /** Where winning bids go (post auction service). Set before start(). */
    void setSubmittedAuctionSink(SubmittedAuctionSink sink);

    void start();   ///< binds the agent socket, starts the event loop thread
    void stop();    ///< joins the thread, safe to call twice

    /** Thread safe. Takes the auction over; always finishes it by its expiry. */
    void injectAuction(std::shared_ptr<Auction> auction);

    /** Thread safe. Forwards to the agent named in event.bid.agent as WIN /
        LOSS / CAMPAIGN_EVENT. Wire these to PostAuctionService handlers. */
    void onMatchedWinLoss(const MatchedWinLoss& event);
    void onMatchedCampaignEvent(const MatchedCampaignEvent& event);

private:
    RouterConfig config_;
    std::shared_ptr<Banker> banker_;
    // The implementation adds its own members (zmq context, thread, queues...).
};

} // namespace RTBKIT

// The "core server": everything except the bidding agents and the exchange,
// in one process, wired the way docs/ARCHITECTURE.md draws it.
//
//   OpenRtbExchangeConnector --> Router --SubmittedAuctionEvent--> PostAuctionService
//        ^                         ^  |                                |  ^
//        |                         |  +--zmq--> bidding agents         |  |
//        |                         +-- MatchedWinLoss / CampaignEvent -+  |
//   (HTTP from exchange)                                     AdServer ----+ (HTTP /win ...)
//
// Config (all sections optional, every one is passed to the component):
//   {"banker":      makeBanker() config (default: empty memory banker),
//    "router":      RouterConfig,
//    "exchange":    OpenRtbExchangeConnector::configure keys,
//    "adserver":    AdServer::configure keys,
//    "postAuction": {"earlyWinTimeoutMs", "finishedWindowMs", "lostWindowMs",
//                    "tickIntervalMs", "maxEntries"},
//    "admin":       {"host": "127.0.0.1", "port": 12370}}   // absent = no admin HTTP
// "exchange.winNoticeUrl" defaults to the ad server's /win on the port it is
// really listening on (so "adserver": {"port": 0} works).
//
// Admin HTTP: GET /stats (statsJson()), GET /ping.
//
// Units: win prices arrive as CPM on /win and become Amounts per impression
// (Amount::fromCpm). The exchange's clearing price is what the banker is
// charged: WinCostModel is not applied on the router side.
#pragma once

#include "adserver/adserver.h"
#include "banker/banker.h"
#include "exchange/openrtb_exchange_connector.h"
#include "post_auction/post_auction.h"
#include "router/router.h"

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

namespace httplib { class Server; }

namespace RTBKIT {

class CoreServer {
public:
    /** Builds and wires the parts. Throws std::runtime_error on a bad config. */
    explicit CoreServer(const nlohmann::json& config = nlohmann::json::object());
    ~CoreServer();

    CoreServer(const CoreServer&) = delete;
    CoreServer& operator=(const CoreServer&) = delete;

    /** Starts post auction, ad server, router, connector (and admin). Throws
        std::runtime_error when a port is taken. Stops what it started then. */
    void start();
    /** Safe to call twice. */
    void stop();

    Banker& banker() { return *banker_; }
    Router& router() { return *router_; }
    PostAuctionService& postAuction() { return *postAuction_; }
    OpenRtbExchangeConnector& connector() { return *connector_; }
    AdServer& adServer() { return *adServer_; }

    /** Valid after start(). */
    std::string agentEndpoint() const { return router_->boundEndpoint(); }
    int exchangePort() const { return connector_->port(); }
    int adServerPort() const { return adServer_->port(); }
    int adminPort() const { return adminPort_; }

    /** {"router": .., "connector": .., "postAuction": .., "banker": ..}. */
    nlohmann::json statsJson() const;

private:
    void startAdmin();
    void stopAll();

    nlohmann::json exchangeConfig_;
    nlohmann::json adminConfig_;
    std::shared_ptr<Banker> banker_;
    std::unique_ptr<PostAuctionService> postAuction_;
    std::unique_ptr<AdServer> adServer_;
    std::unique_ptr<Router> router_;
    std::shared_ptr<OpenRtbExchangeConnector> connector_;

    std::unique_ptr<httplib::Server> admin_;
    std::thread adminThread_;
    int adminPort_ = 0;
    bool started_ = false;
};

} // namespace RTBKIT

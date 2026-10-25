// The ad server: the HTTP endpoint that receives win notices, impression
// pixels and clicks, and turns them into PostAuctionEvents for the post
// auction service.
//
// HTTP contract (query string; the win notice URL is put into the OpenRTB bid
// response `nurl` by the exchange connector, see docs/ARCHITECTURE.md):
//   GET /win?auctionId=<id>&adSpotId=<id>&price=<cpm>[&currency=USD]
//        price is the OpenRTB ${AUCTION_PRICE} macro: CPM in currency units
//        (1.5 == 1.5 USD per thousand) -> Amount::fromCpm(), i.e. micro
//        units per impression, the same unit as Bid prices.
//   GET /impression?auctionId=<id>&adSpotId=<id>
//   GET /click?auctionId=<id>&adSpotId=<id>
//   GET /ping            -> 200 "pong"
//   GET /stats           -> 200 JSON, PostAuctionService::statsJson()
// Replies 200 "ok" for a valid event (even if it later matches nothing), 400
// for missing or bad parameters (non-numeric, negative or non-finite price,
// unknown currency).
#pragma once

#include "common/events.h"
#include "post_auction/post_auction.h"

#include <memory>
#include <nlohmann/json.hpp>
#include <string>

namespace RTBKIT {

class AdServer {
public:
    explicit AdServer(PostAuctionService& postAuction);
    ~AdServer();

    AdServer(const AdServer&) = delete;
    AdServer& operator=(const AdServer&) = delete;

    /** {"port": 12360, "host": "127.0.0.1", "threads": 8}. Port 0 picks a free
        port (read it with port() after start()). Throws std::runtime_error. */
    void configure(const nlohmann::json& config);

    /** Returns when listening. Throws std::runtime_error if it cannot bind. */
    void start();
    /** Joins the server thread, safe to call twice. */
    void stop();
    /** The port in use once started, else the configured one. */
    int port() const;

private:
    struct Impl;

    PostAuctionService& postAuction_;
    std::string host_ = "127.0.0.1";
    int port_ = 12360;
    size_t threads_ = 8;
    std::unique_ptr<Impl> impl_;
};

} // namespace RTBKIT

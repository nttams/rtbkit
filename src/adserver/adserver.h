// Interface sketch of the ad server: the HTTP endpoint that receives win
// notices, impression pixels and clicks, and turns them into PostAuctionEvents
// for the post auction service.
//
// HTTP contract (query string; the win notice URL is put into the OpenRTB bid
// response `nurl` by the exchange connector, see docs/ARCHITECTURE.md):
//   GET /win?auctionId=<id>&adSpotId=<id>&price=<cpm>[&currency=USD]
//        price is the OpenRTB ${AUCTION_PRICE} macro: CPM in currency units
//        (1.5 == 1.5 USD per thousand) -> Amount::fromCpm()
//   GET /impression?auctionId=<id>&adSpotId=<id>
//   GET /click?auctionId=<id>&adSpotId=<id>
//   GET /ping            -> 200 "pong"
// Replies 200 for a valid event (even if it later matches nothing), 400 for
// missing or bad parameters.
#pragma once

#include "common/events.h"
#include "post_auction/post_auction.h"

#include <nlohmann/json.hpp>

namespace RTBKIT {

class AdServer {
public:
    explicit AdServer(PostAuctionService& postAuction);

    /** {"port": 12360, "host": "127.0.0.1"}. Throws std::runtime_error. */
    void configure(const nlohmann::json& config);

    void start();   ///< returns when listening
    void stop();    ///< joins the server thread, safe to call twice
    int port() const;

private:
    PostAuctionService& postAuction_;
    // The implementation adds its own members (httplib::Server, thread...).
};

} // namespace RTBKIT

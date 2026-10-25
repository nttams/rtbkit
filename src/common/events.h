// Events that flow after the router has answered an auction. Simplified from
// rtbkit/common/auction_events.h.
//
//   router      --SubmittedAuctionEvent-->  post auction
//   ad server   --PostAuctionEvent------->  post auction  (win notice, click..)
//   post auction --MatchedWinLoss / MatchedCampaignEvent--> router --> agent
#pragma once

#include "common/auction.h"
#include "common/bid_request.h"
#include "common/bid_request_json.h"
#include "common/currency.h"
#include "common/id.h"

#include <memory>
#include <nlohmann/json.hpp>
#include <string>

namespace RTBKIT {

/** Item id used with the Banker for the bid on one spot of one auction. The
    router (authorizeBid / cancelBid) and the post auction service (winBid)
    must agree on it. */
inline std::string bidItemId(const Id& auctionId, const Id& adSpotId)
{
    return auctionId.toString() + ":" + adSpotId.toString();
}

/** Router -> post auction: "we answered this spot with this bid". */
struct SubmittedAuctionEvent {
    Id auctionId;
    Id adSpotId;                          ///< AdSpot::id of the spot we bid on

    /** If no win notice arrives before this, the bid is a loss. */
    Date lossTimeout;

    /** The winning response we sent to the exchange. */
    Response bid;

    /** Optional: the request, kept for reference. */
    std::shared_ptr<const BidRequest> request;

    nlohmann::json toJson() const;
    /** Throws std::runtime_error. */
    static SubmittedAuctionEvent fromJson(const nlohmann::json& j);
};

enum class PostAuctionEventType { WIN, LOSS, CAMPAIGN_EVENT };
enum class CampaignEventType { IMPRESSION, CLICK };

/** "WIN", "LOSS", "CAMPAIGN_EVENT". parse throws std::runtime_error. */
std::string toString(PostAuctionEventType type);
PostAuctionEventType parsePostAuctionEventType(const std::string& name);

/** "IMPRESSION", "CLICK". parse throws std::runtime_error. */
std::string toString(CampaignEventType type);
CampaignEventType parseCampaignEventType(const std::string& name);

/** Ad server (or exchange) -> post auction. */
struct PostAuctionEvent {
    PostAuctionEventType type = PostAuctionEventType::WIN;
    Id auctionId;
    Id adSpotId;
    Date timestamp;

    /** WIN only: the clearing price per impression, as told by the exchange. */
    Amount winPrice;

    /** CAMPAIGN_EVENT only. */
    CampaignEventType campaignEvent = CampaignEventType::IMPRESSION;

    nlohmann::json meta;

    nlohmann::json toJson() const;
    static PostAuctionEvent fromJson(const nlohmann::json& j);
};

/** Post auction -> router -> agent: a submitted bid was matched to its result. */
struct MatchedWinLoss {
    enum class Type {
        WIN,
        LATE_WIN,   ///< win notice arrived after the loss timeout
        LOSS
    };

    Type type = Type::LOSS;
    Id auctionId;
    Id adSpotId;
    Date timestamp;

    /** What was paid per impression; zero for a loss. */
    Amount winPrice;

    /** The bid we made (agent, account, bidData, meta...). */
    Response bid;

    nlohmann::json toJson() const;
    static MatchedWinLoss fromJson(const nlohmann::json& j);
};

std::string toString(MatchedWinLoss::Type type);
MatchedWinLoss::Type parseMatchedWinLossType(const std::string& name);

/** Post auction -> router -> agent: an impression or click on a won bid. */
struct MatchedCampaignEvent {
    CampaignEventType event = CampaignEventType::IMPRESSION;
    Id auctionId;
    Id adSpotId;
    Date timestamp;
    Response bid;

    nlohmann::json toJson() const;
    static MatchedCampaignEvent fromJson(const nlohmann::json& j);
};

} // namespace RTBKIT

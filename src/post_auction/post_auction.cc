#include "post_auction/post_auction.h"

namespace RTBKIT {

PostAuctionService::PostAuctionService(std::shared_ptr<Banker> banker)
    : banker_(std::move(banker))
{
}

void PostAuctionService::setWinLossHandler(WinLossHandler handler)
{
    winLossHandler_ = std::move(handler);
}

void PostAuctionService::setCampaignEventHandler(CampaignEventHandler handler)
{
    campaignEventHandler_ = std::move(handler);
}

} // namespace RTBKIT

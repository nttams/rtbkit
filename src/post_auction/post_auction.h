// Interface sketch of the post auction service: matches win / loss notices
// and campaign events to the bids the router submitted, settles the money with
// the banker, and tells the router (which tells the agents) what happened.
//
// Flow, per submitted bid (item id = bidItemId(auctionId, adSpotId)):
//   onSubmittedAuction(e)   remember e until e.lossTimeout (bid is still
//                           "in flight" at the banker)
//   onWin(event)            banker.winBid(account, itemId, event.winPrice);
//                           emit MatchedWinLoss{WIN} (LATE_WIN if the loss
//                           timeout already passed: banker.forceWinBid)
//   tick(now) after timeout banker.cancelBid(...); emit MatchedWinLoss{LOSS}
//   onLoss(event)           same as a timeout, but immediate
//   onCampaignEvent(event)  if the bid was won: emit MatchedCampaignEvent
// Events that match nothing are counted and dropped.
//
// Threading: all methods may be called from any thread (ad server HTTP
// threads, router thread, a timer thread calling tick()).
#pragma once

#include "banker/banker.h"
#include "common/events.h"

#include <cstddef>
#include <functional>
#include <memory>

namespace RTBKIT {

class PostAuctionService {
public:
    using WinLossHandler = std::function<void(const MatchedWinLoss&)>;
    using CampaignEventHandler = std::function<void(const MatchedCampaignEvent&)>;

    explicit PostAuctionService(std::shared_ptr<Banker> banker);

    /** Where matched results go (the router, in the demo). Set before use. */
    void setWinLossHandler(WinLossHandler handler);
    void setCampaignEventHandler(CampaignEventHandler handler);

    /** From the router, for every winning response it sent to an exchange. */
    void onSubmittedAuction(const SubmittedAuctionEvent& event);

    /** From the ad server. */
    void onWin(const PostAuctionEvent& event);
    void onLoss(const PostAuctionEvent& event);
    void onCampaignEvent(const PostAuctionEvent& event);

    /** Dispatches on event.type to one of the three above. */
    void onEvent(const PostAuctionEvent& event);

    /** Turns every submitted bid whose lossTimeout is before `now` into a
        loss. Call it periodically. */
    void tick(Date now);

    size_t numSubmitted() const;   ///< bids waiting for a result
    size_t numUnmatched() const;   ///< events that matched no submitted bid

    Banker& banker() { return *banker_; }

private:
    std::shared_ptr<Banker> banker_;
    WinLossHandler winLossHandler_;
    CampaignEventHandler campaignEventHandler_;
    // The implementation adds its own members (submitted-bid table, mutex...).
};

} // namespace RTBKIT

// Post auction service: matches win / loss notices and campaign events to the
// bids the router submitted, settles the money with the banker, and tells the
// router (which tells the agents) what happened.
//
// Flow, per submitted bid (item id = bidItemId(auctionId, adSpotId)):
//   onSubmittedAuction(e)   remember e until e.lossTimeout (bid is still
//                           "in flight" at the banker). A win notice that
//                           arrived earlier is matched now.
//   onWin(event)            banker.winBid(account, itemId, event.winPrice);
//                           emit MatchedWinLoss{WIN}; the bid then stays in a
//                           "finished" table to match campaign events.
//                           After the loss timeout: banker.forceWinBid and
//                           MatchedWinLoss{LATE_WIN}.
//                           A win for an unknown bid is buffered for
//                           Options::earlyWinTimeout (the win can overtake the
//                           router's submission), then dropped as unmatched.
//                           A second win for the same bid is ignored.
//   tick(now) after timeout banker.cancelBid(...); emit MatchedWinLoss{LOSS}
//   onLoss(event)           same as a timeout, but immediate
//   onCampaignEvent(event)  if the bid was won: emit MatchedCampaignEvent
// Events that match nothing are counted and dropped.
// Bids with Response::test set are never charged: the banker only gets a
// cancelBid (which releases a reservation, if there is one).
//
// Threading: all methods may be called from any thread (ad server HTTP
// threads, router thread, a timer thread calling tick()). Handlers and banker
// calls run on the calling thread without any internal lock held, so a
// handler may call back into the service.
#pragma once

#include "banker/banker.h"
#include "common/events.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

namespace RTBKIT {

class PostAuctionService {
public:
    using WinLossHandler = std::function<void(const MatchedWinLoss&)>;
    using CampaignEventHandler = std::function<void(const MatchedCampaignEvent&)>;

    struct Options {
        /** How long a win that arrived before its submission is kept. */
        std::chrono::milliseconds earlyWinTimeout{10'000};
        /** How long a won bid is kept to match impressions and clicks. */
        std::chrono::milliseconds finishedWindow{std::chrono::hours(1)};
        /** How long a lost bid is kept to recognise a late win. */
        std::chrono::milliseconds lostWindow{std::chrono::hours(1)};
        /** Period of the optional timer thread. */
        std::chrono::milliseconds tickInterval{100};
    };

    explicit PostAuctionService(std::shared_ptr<Banker> banker);
    PostAuctionService(std::shared_ptr<Banker> banker, Options options);
    ~PostAuctionService();

    PostAuctionService(const PostAuctionService&) = delete;
    PostAuctionService& operator=(const PostAuctionService&) = delete;

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
        loss, and forgets buffered wins and old finished bids. Call it
        periodically (or use start()). */
    void tick(Date now);

    /** Optional timer thread calling tick(now) every Options::tickInterval.
        start() twice and stop() twice are harmless. */
    void start();
    void stop();

    size_t numSubmitted() const;   ///< bids waiting for a result
    size_t numUnmatched() const;   ///< events that matched no submitted bid

    /** {"submitted": total received, "pending": waiting for a result,
        "wins", "lateWins", "losses", "duplicateWins", "unmatched",
        "campaignEvents", "bankerErrors", "finished", "earlyWins": buffered now}. */
    nlohmann::json statsJson() const;

    Banker& banker() { return *banker_; }

private:
    struct Pending {
        SubmittedAuctionEvent event;
    };
    struct EarlyWin {
        PostAuctionEvent event;
        Date expiry;
    };
    struct Settled {
        Response bid;
        Date timestamp;
        Amount winPrice;   // zero for a lost bid
        Date expiry;
    };
    struct Counters {
        size_t submitted = 0, wins = 0, lateWins = 0, losses = 0,
               duplicateWins = 0, unmatched = 0, campaignEvents = 0,
               bankerErrors = 0;
    };

    /** Side effects of a state change (banker call, then the handler); the
        state itself was already changed under the lock. */
    void finishWin(const Id& auctionId, const Id& adSpotId, const Settled& won,
                   MatchedWinLoss::Type type);
    void finishLoss(const Id& auctionId, const Id& adSpotId, const Settled& lost);
    /** Runs a banker call, counting (not propagating) its exceptions. */
    void bankerCall(const std::function<void()>& call);

    std::shared_ptr<Banker> banker_;
    Options options_;
    WinLossHandler winLossHandler_;
    CampaignEventHandler campaignEventHandler_;

    mutable std::mutex mutex_;
    std::map<std::string, Pending> submitted_;
    std::map<std::string, EarlyWin> earlyWins_;
    std::map<std::string, Settled> finished_;   // won
    std::map<std::string, Settled> lost_;       // lost or timed out
    Counters counters_;

    std::mutex timerMutex_;
    std::condition_variable timerWake_;
    std::thread timer_;
    bool timerStop_ = false;
};

} // namespace RTBKIT

// Post auction service: matches win / loss notices and campaign events to the
// bids the router submitted, settles the money with the banker, and tells the
// router (which tells the agents) what happened.
//
// Flow, per submitted bid (banker item id = bidItemId(auctionId, adSpotId, bid.agent)):
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
// The price charged is min(win price, bid price): an exchange (or anybody who
// can reach the unauthenticated /win endpoint) cannot make us pay more than
// we bid. Clamped notices are counted in stats ("clamped").
// Memory: finished and lost bids are kept as compact records (the agent config
// and request are dropped) for Options windows, and the tables (finished, lost,
// early wins) never hold more than Options::maxEntries; the oldest go first.
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
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <unordered_map>

namespace RTBKIT {

class PostAuctionService {
public:
    using WinLossHandler = std::function<void(const MatchedWinLoss&)>;
    using CampaignEventHandler = std::function<void(const MatchedCampaignEvent&)>;

    struct Options {
        /** How long a win that arrived before its submission is kept. */
        std::chrono::milliseconds earlyWinTimeout{10'000};
        /** How long a won bid is kept to match impressions and clicks. */
        std::chrono::milliseconds finishedWindow{std::chrono::minutes(10)};
        /** How long a lost bid is kept to recognise a late win. */
        std::chrono::milliseconds lostWindow{std::chrono::minutes(10)};
        /** Hard cap on each of the finished, lost and early win tables; the
            oldest entry is dropped (and counted in "evicted") beyond it. */
        size_t maxEntries = 1'000'000;
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

    /** For shutdown, after the router stopped feeding us: cancels the banker
        reservation of every bid still waiting for a result (counted as
        "flushed"), and forgets buffered early wins. No handlers are called. */
    void flushPending();

    size_t numSubmitted() const;   ///< bids waiting for a result
    size_t numUnmatched() const;   ///< events that matched no submitted bid

    /** {"submitted": total received, "pending": waiting for a result,
        "wins", "lateWins", "losses", "duplicateWins", "unmatched",
        "campaignEvents", "bankerErrors", "clamped": win prices above the bid
        price, "evicted": entries dropped by the maxEntries cap, "flushed": reservations
        released by flushPending, "finished",
        "lost", "earlyWins": kept now}. */
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
        Response bid;      // compact: Response::config is dropped
        Date timestamp;
        Amount winPrice;   // zero for a lost bid
        Date expiry;
    };
    struct Counters {
        size_t submitted = 0, wins = 0, lateWins = 0, losses = 0,
               duplicateWins = 0, unmatched = 0, campaignEvents = 0,
               bankerErrors = 0, clamped = 0, evicted = 0, flushed = 0;
    };

    /** Insertion ordered table with a size cap: inserting into a full table
        drops the oldest entry. T has a `Date expiry`. Not thread safe
        (guarded by mutex_). */
    template<typename T>
    class OrderedTable {
    public:
        explicit OrderedTable(size_t cap) : cap_(cap == 0 ? 1 : cap) {}

        size_t size() const { return entries_.size(); }
        bool contains(const std::string& key) const { return index_.contains(key); }

        T* find(const std::string& key)
        {
            auto it = index_.find(key);
            return it == index_.end() ? nullptr : &it->second->second;
        }

        /** Returns how many old entries were evicted to make room. */
        size_t insert(const std::string& key, T value)
        {
            erase(key);
            if (!entries_.empty() && value.expiry < entries_.back().second.expiry)
                value.expiry = entries_.back().second.expiry;
            entries_.emplace_back(key, std::move(value));
            index_[key] = std::prev(entries_.end());
            size_t evicted = 0;
            while (entries_.size() > cap_) {
                index_.erase(entries_.front().first);
                entries_.pop_front();
                ++evicted;
            }
            return evicted;
        }

        void erase(const std::string& key)
        {
            auto it = index_.find(key);
            if (it == index_.end()) return;
            entries_.erase(it->second);
            index_.erase(it);
        }

        /** Drops the entries whose expiry is before `now`; returns how many.
            Only the front is looked at: insert() keeps expiry non-decreasing
            along the table (windows are constant per table, and an entry
            inserted with an earlier expiry than the last one, because of
            clock jitter between threads, is raised to it: it may live a
            moment longer, never shorter). */
        size_t popExpired(Date now)
        {
            size_t popped = 0;
            while (!entries_.empty() && entries_.front().second.expiry < now) {
                index_.erase(entries_.front().first);
                entries_.pop_front();
                ++popped;
            }
            return popped;
        }

    private:
        size_t cap_;
        std::list<std::pair<std::string, T>> entries_;
        std::unordered_map<std::string, typename std::list<std::pair<std::string, T>>::iterator> index_;
    };

    /** Side effects of a state change (banker call, then the handler); the
        state itself was already changed under the lock. */
    void finishWin(const Id& auctionId, const Id& adSpotId, const Settled& won,
                   MatchedWinLoss::Type type);
    void finishLoss(const Id& auctionId, const Id& adSpotId, const Settled& lost);
    /** The amount to charge for a win notice: the win price, but never above
        the bid price (or negative). Counts a change in counters_.clamped; call
        with mutex_ held. */
    Amount chargedPrice(const Response& bid, const Amount& winPrice);
    /** The response without its config (which pins a whole AgentConfig). */
    static Response compact(Response bid);
    /** Runs a banker call, counting (not propagating) its exceptions. */
    void bankerCall(const std::function<void()>& call);

    std::shared_ptr<Banker> banker_;
    Options options_;
    WinLossHandler winLossHandler_;
    CampaignEventHandler campaignEventHandler_;

    mutable std::mutex mutex_;
    std::map<std::string, Pending> submitted_;
    OrderedTable<EarlyWin> earlyWins_;
    OrderedTable<Settled> finished_;   // won
    OrderedTable<Settled> lost_;       // lost or timed out
    Counters counters_;

    std::mutex timerMutex_;
    std::condition_variable timerWake_;
    std::thread timer_;
    bool timerStop_ = false;
};

} // namespace RTBKIT

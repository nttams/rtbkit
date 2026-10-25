#include "post_auction/post_auction.h"

#include <optional>
#include <utility>
#include <vector>

namespace RTBKIT {

namespace {

/** Table key: win notices carry only auction and spot (one winner per spot). */
std::string key(const Id& auctionId, const Id& adSpotId)
{
    return auctionSpotKey(auctionId, adSpotId);
}

Date orNow(Date when)
{
    return when == Date() ? std::chrono::system_clock::now() : when;
}

} // namespace

PostAuctionService::PostAuctionService(std::shared_ptr<Banker> banker)
    : PostAuctionService(std::move(banker), Options())
{
}

PostAuctionService::PostAuctionService(std::shared_ptr<Banker> banker, Options options)
    : banker_(std::move(banker)), options_(options), earlyWins_(options.maxEntries),
      finished_(options.maxEntries), lost_(options.maxEntries)
{
}

PostAuctionService::~PostAuctionService()
{
    stop();
}

void PostAuctionService::setWinLossHandler(WinLossHandler handler)
{
    winLossHandler_ = std::move(handler);
}

void PostAuctionService::setCampaignEventHandler(CampaignEventHandler handler)
{
    campaignEventHandler_ = std::move(handler);
}

Response PostAuctionService::compact(Response bid)
{
    bid.config.reset();
    return bid;
}

Amount PostAuctionService::chargedPrice(const Response& bid, const Amount& winPrice)
{
    Amount charged = winPrice;
    if (!winPrice.currencyIsCompatible(bid.price) || winPrice.isNegative())
        charged = winPrice.isNegative() ? Amount() : bid.price;
    else if (winPrice > bid.price)
        charged = bid.price;
    if (charged != winPrice) ++counters_.clamped;
    return charged;
}

void PostAuctionService::bankerCall(const std::function<void()>& call)
{
    try {
        call();
    }
    catch (const std::exception&) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++counters_.bankerErrors;
    }
}

void PostAuctionService::finishWin(const Id& auctionId, const Id& adSpotId,
                                   const Settled& won, MatchedWinLoss::Type type)
{
    const Response& bid = won.bid;
    const std::string item = bidItemId(auctionId, adSpotId, bid.agent);
    if (bid.test) {
        bankerCall([&] { banker_->cancelBid(bid.account, item); });
    }
    else if (type == MatchedWinLoss::Type::LATE_WIN) {
        bankerCall([&] { banker_->forceWinBid(bid.account, item, won.winPrice); });
    }
    else {
        bankerCall([&] { banker_->winBid(bid.account, item, won.winPrice); });
    }

    if (!winLossHandler_) return;
    MatchedWinLoss result;
    result.type = type;
    result.auctionId = auctionId;
    result.adSpotId = adSpotId;
    result.timestamp = won.timestamp;
    result.winPrice = won.winPrice;
    result.bid = bid;
    winLossHandler_(result);
}

void PostAuctionService::finishLoss(const Id& auctionId, const Id& adSpotId,
                                    const Settled& lost)
{
    const std::string item = bidItemId(auctionId, adSpotId, lost.bid.agent);
    bankerCall([&] { banker_->cancelBid(lost.bid.account, item); });

    if (!winLossHandler_) return;
    MatchedWinLoss result;
    result.type = MatchedWinLoss::Type::LOSS;
    result.auctionId = auctionId;
    result.adSpotId = adSpotId;
    result.timestamp = lost.timestamp;
    result.bid = lost.bid;
    winLossHandler_(result);
}

void PostAuctionService::onSubmittedAuction(const SubmittedAuctionEvent& event)
{
    const std::string item = key(event.auctionId, event.adSpotId);
    const Date now = std::chrono::system_clock::now();

    std::optional<Settled> won;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++counters_.submitted;
        auto* early = earlyWins_.find(item);
        if (!early) {
            submitted_[item] = Pending{event};
            return;
        }
        Settled settled{compact(event.bid), orNow(early->event.timestamp),
                        chargedPrice(event.bid, early->event.winPrice),
                        now + options_.finishedWindow};
        earlyWins_.erase(item);
        counters_.evicted += finished_.insert(item, settled);
        ++counters_.wins;
        won = std::move(settled);
    }
    finishWin(event.auctionId, event.adSpotId, *won, MatchedWinLoss::Type::WIN);
}

void PostAuctionService::onWin(const PostAuctionEvent& event)
{
    const std::string item = key(event.auctionId, event.adSpotId);
    const Date now = std::chrono::system_clock::now();

    Settled won;
    MatchedWinLoss::Type type = MatchedWinLoss::Type::WIN;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto it = submitted_.find(item); it != submitted_.end()) {
            won = Settled{compact(it->second.event.bid), orNow(event.timestamp),
                          chargedPrice(it->second.event.bid, event.winPrice),
                          now + options_.finishedWindow};
            submitted_.erase(it);
            ++counters_.wins;
        }
        else if (finished_.contains(item)) {
            ++counters_.duplicateWins;
            return;
        }
        else if (auto* lostBid = lost_.find(item)) {
            won = Settled{lostBid->bid, orNow(event.timestamp),
                          chargedPrice(lostBid->bid, event.winPrice),
                          now + options_.finishedWindow};
            lost_.erase(item);
            type = MatchedWinLoss::Type::LATE_WIN;
            ++counters_.lateWins;
        }
        else if (earlyWins_.contains(item)) {
            ++counters_.duplicateWins;
            return;
        }
        else {
            counters_.evicted += earlyWins_.insert(
                item, EarlyWin{event, now + options_.earlyWinTimeout});
            return;
        }
        counters_.evicted += finished_.insert(item, won);
    }
    finishWin(event.auctionId, event.adSpotId, won, type);
}

void PostAuctionService::onLoss(const PostAuctionEvent& event)
{
    const std::string item = key(event.auctionId, event.adSpotId);
    const Date now = std::chrono::system_clock::now();

    Settled lost;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = submitted_.find(item);
        if (it == submitted_.end()) {
            // A repeated loss for a bid we already settled is not an error.
            if (!finished_.contains(item) && !lost_.contains(item)) ++counters_.unmatched;
            return;
        }
        lost = Settled{compact(it->second.event.bid), orNow(event.timestamp), Amount(),
                       now + options_.lostWindow};
        submitted_.erase(it);
        counters_.evicted += lost_.insert(item, lost);
        ++counters_.losses;
    }
    finishLoss(event.auctionId, event.adSpotId, lost);
}

void PostAuctionService::onCampaignEvent(const PostAuctionEvent& event)
{
    const std::string item = key(event.auctionId, event.adSpotId);

    MatchedCampaignEvent matched;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* won = finished_.find(item);
        if (!won) {
            ++counters_.unmatched;
            return;
        }
        matched.bid = won->bid;
        ++counters_.campaignEvents;
    }
    matched.event = event.campaignEvent;
    matched.auctionId = event.auctionId;
    matched.adSpotId = event.adSpotId;
    matched.timestamp = orNow(event.timestamp);
    if (campaignEventHandler_) campaignEventHandler_(matched);
}

void PostAuctionService::onEvent(const PostAuctionEvent& event)
{
    switch (event.type) {
    case PostAuctionEventType::WIN: onWin(event); break;
    case PostAuctionEventType::LOSS: onLoss(event); break;
    case PostAuctionEventType::CAMPAIGN_EVENT: onCampaignEvent(event); break;
    }
}

void PostAuctionService::tick(Date now)
{
    struct Timeout {
        Id auctionId, adSpotId;
        Settled lost;
    };
    std::vector<Timeout> timeouts;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = submitted_.begin(); it != submitted_.end();) {
            const SubmittedAuctionEvent& e = it->second.event;
            if (e.lossTimeout >= now) {
                ++it;
                continue;
            }
            Settled lost{compact(e.bid), now, Amount(), now + options_.lostWindow};
            counters_.evicted += lost_.insert(it->first, lost);
            timeouts.push_back({e.auctionId, e.adSpotId, std::move(lost)});
            ++counters_.losses;
            it = submitted_.erase(it);
        }
        counters_.unmatched += earlyWins_.popExpired(now);
        finished_.popExpired(now);
        lost_.popExpired(now);
    }
    for (const Timeout& t : timeouts) finishLoss(t.auctionId, t.adSpotId, t.lost);
}

void PostAuctionService::start()
{
    std::lock_guard<std::mutex> lock(timerMutex_);
    if (timer_.joinable()) return;
    timerStop_ = false;
    timer_ = std::thread([this] {
        std::unique_lock<std::mutex> lock(timerMutex_);
        while (!timerStop_) {
            if (timerWake_.wait_for(lock, options_.tickInterval,
                                    [this] { return timerStop_; })) {
                break;
            }
            lock.unlock();
            tick(std::chrono::system_clock::now());
            lock.lock();
        }
    });
}

void PostAuctionService::stop()
{
    std::thread timer;
    {
        std::lock_guard<std::mutex> lock(timerMutex_);
        timerStop_ = true;
        timer = std::move(timer_);
    }
    timerWake_.notify_all();
    if (timer.joinable()) timer.join();
}

void PostAuctionService::flushPending()
{
    std::vector<SubmittedAuctionEvent> pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [item, entry] : submitted_) pending.push_back(std::move(entry.event));
        submitted_.clear();
        counters_.flushed += pending.size();
        earlyWins_.popExpired(Date::max());
    }
    for (const auto& e : pending) {
        const std::string item = bidItemId(e.auctionId, e.adSpotId, e.bid.agent);
        bankerCall([&] { banker_->cancelBid(e.bid.account, item); });
    }
}

size_t PostAuctionService::numSubmitted() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return submitted_.size();
}

size_t PostAuctionService::numUnmatched() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return counters_.unmatched;
}

nlohmann::json PostAuctionService::statsJson() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return {
        {"submitted", counters_.submitted},
        {"pending", submitted_.size()},
        {"wins", counters_.wins},
        {"lateWins", counters_.lateWins},
        {"losses", counters_.losses},
        {"duplicateWins", counters_.duplicateWins},
        {"unmatched", counters_.unmatched},
        {"campaignEvents", counters_.campaignEvents},
        {"bankerErrors", counters_.bankerErrors},
        {"clamped", counters_.clamped},
        {"evicted", counters_.evicted},
        {"flushed", counters_.flushed},
        {"finished", finished_.size()},
        {"lost", lost_.size()},
        {"earlyWins", earlyWins_.size()},
    };
}

} // namespace RTBKIT

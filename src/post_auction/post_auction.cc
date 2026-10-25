#include "post_auction/post_auction.h"

#include <optional>
#include <utility>
#include <vector>

namespace RTBKIT {

namespace {

std::string key(const Id& auctionId, const Id& adSpotId)
{
    return bidItemId(auctionId, adSpotId);
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
    : banker_(std::move(banker)), options_(options)
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
    const std::string item = key(auctionId, adSpotId);
    const Response& bid = won.bid;
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
    const std::string item = key(auctionId, adSpotId);
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
        auto early = earlyWins_.find(item);
        if (early == earlyWins_.end()) {
            submitted_[item] = Pending{event};
            return;
        }
        Settled settled{event.bid, orNow(early->second.event.timestamp),
                        early->second.event.winPrice, now + options_.finishedWindow};
        earlyWins_.erase(early);
        finished_[item] = settled;
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
            won = Settled{it->second.event.bid, orNow(event.timestamp), event.winPrice,
                          now + options_.finishedWindow};
            submitted_.erase(it);
            ++counters_.wins;
        }
        else if (finished_.count(item)) {
            ++counters_.duplicateWins;
            return;
        }
        else if (auto lostIt = lost_.find(item); lostIt != lost_.end()) {
            won = Settled{lostIt->second.bid, orNow(event.timestamp), event.winPrice,
                          now + options_.finishedWindow};
            lost_.erase(lostIt);
            type = MatchedWinLoss::Type::LATE_WIN;
            ++counters_.lateWins;
        }
        else if (earlyWins_.count(item)) {
            ++counters_.duplicateWins;
            return;
        }
        else {
            earlyWins_[item] = EarlyWin{event, now + options_.earlyWinTimeout};
            return;
        }
        finished_[item] = won;
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
            if (!finished_.count(item) && !lost_.count(item)) ++counters_.unmatched;
            return;
        }
        lost = Settled{it->second.event.bid, orNow(event.timestamp), Amount(),
                       now + options_.lostWindow};
        submitted_.erase(it);
        lost_[item] = lost;
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
        auto it = finished_.find(item);
        if (it == finished_.end()) {
            ++counters_.unmatched;
            return;
        }
        matched.bid = it->second.bid;
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
            Settled lost{e.bid, now, Amount(), now + options_.lostWindow};
            lost_[it->first] = lost;
            timeouts.push_back({e.auctionId, e.adSpotId, std::move(lost)});
            ++counters_.losses;
            it = submitted_.erase(it);
        }
        for (auto it = earlyWins_.begin(); it != earlyWins_.end();) {
            if (it->second.expiry < now) {
                ++counters_.unmatched;
                it = earlyWins_.erase(it);
            }
            else ++it;
        }
        std::erase_if(finished_, [&](const auto& kv) { return kv.second.expiry < now; });
        std::erase_if(lost_, [&](const auto& kv) { return kv.second.expiry < now; });
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
        {"finished", finished_.size()},
        {"earlyWins", earlyWins_.size()},
    };
}

} // namespace RTBKIT

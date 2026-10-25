#include "post_auction/post_auction.h"

#include "fixtures.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace RTBKIT;
using namespace RTBKIT::Test;
using namespace std::chrono_literals;

namespace {

/** Banker that records every settlement call. */
class RecordingBanker : public NullBanker {
public:
    struct Call {
        std::string what;   // "win", "cancel", "forceWin"
        std::string account;
        std::string item;
        Amount paid;
    };

    void cancelBid(const AccountKey& account, const std::string& item) override
    {
        record({"cancel", account.toString(), item, Amount()});
    }
    void winBid(const AccountKey& account, const std::string& item,
                const Amount& paid) override
    {
        record({"win", account.toString(), item, paid});
    }
    void forceWinBid(const AccountKey& account, const std::string& item,
                     const Amount& paid) override
    {
        record({"forceWin", account.toString(), item, paid});
    }

    std::vector<Call> calls() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return calls_;
    }

private:
    void record(Call call)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        calls_.push_back(std::move(call));
    }
    mutable std::mutex mutex_;
    std::vector<Call> calls_;
};

struct Fixture {
    std::shared_ptr<RecordingBanker> banker = std::make_shared<RecordingBanker>();
    PostAuctionService service;
    std::vector<MatchedWinLoss> results;
    std::vector<MatchedCampaignEvent> campaign;

    explicit Fixture(PostAuctionService::Options options = {})
        : service(banker, options)
    {
        service.setWinLossHandler([this](const MatchedWinLoss& r) { results.push_back(r); });
        service.setCampaignEventHandler(
            [this](const MatchedCampaignEvent& e) { campaign.push_back(e); });
    }
};

Date now() { return std::chrono::system_clock::now(); }

SubmittedAuctionEvent submission(const std::string& auction = "a1", uint64_t spot = 1,
                                 Date timeout = now() + 1h)
{
    SubmittedAuctionEvent e;
    e.auctionId = Id(auction);
    e.adSpotId = Id(spot);
    e.lossTimeout = timeout;
    e.bid = sampleResponse(0, 2.0);
    return e;
}

PostAuctionEvent event(PostAuctionEventType type, const std::string& auction = "a1",
                       uint64_t spot = 1)
{
    PostAuctionEvent e;
    e.type = type;
    e.auctionId = Id(auction);
    e.adSpotId = Id(spot);
    e.timestamp = now();
    return e;
}

PostAuctionEvent win(double cpm, const std::string& auction = "a1", uint64_t spot = 1)
{
    PostAuctionEvent e = event(PostAuctionEventType::WIN, auction, spot);
    e.winPrice = USD_CPM(cpm);
    return e;
}

PostAuctionEvent campaignEvent(CampaignEventType type, const std::string& auction = "a1")
{
    PostAuctionEvent e = event(PostAuctionEventType::CAMPAIGN_EVENT, auction);
    e.campaignEvent = type;
    return e;
}

} // namespace

TEST_CASE("post auction: constructed on a banker")
{
    auto banker = std::make_shared<NullBanker>();
    PostAuctionService service(banker);
    CHECK(&service.banker() == banker.get());
    CHECK(service.numSubmitted() == 0);
    CHECK(service.numUnmatched() == 0);
}

TEST_CASE("post auction: win matches the submission and charges the banker")
{
    Fixture f;
    f.service.onSubmittedAuction(submission());
    CHECK(f.service.numSubmitted() == 1);

    f.service.onEvent(win(1.5));

    CHECK(f.service.numSubmitted() == 0);
    REQUIRE(f.banker->calls().size() == 1);
    CHECK(f.banker->calls()[0].what == "win");
    CHECK(f.banker->calls()[0].account == "campaign:strategy");
    CHECK(f.banker->calls()[0].item == "a1:1:agent1");
    CHECK(f.banker->calls()[0].paid == USD_CPM(1.5));

    REQUIRE(f.results.size() == 1);
    CHECK(f.results[0].type == MatchedWinLoss::Type::WIN);
    CHECK(f.results[0].auctionId == Id("a1"));
    CHECK(f.results[0].adSpotId == Id(uint64_t(1)));
    CHECK(f.results[0].winPrice == USD_CPM(1.5));
    CHECK(f.results[0].bid.agent == "agent1");
    CHECK(f.results[0].bid.bidData == "data-agent1");

    auto stats = f.service.statsJson();
    CHECK(stats["submitted"] == 1);
    CHECK(stats["wins"] == 1);
    CHECK(stats["pending"] == 0);
}

TEST_CASE("post auction: win before submission is matched when it arrives")
{
    Fixture f;
    f.service.onWin(win(1.0));
    CHECK(f.results.empty());
    CHECK(f.banker->calls().empty());
    CHECK(f.service.statsJson()["earlyWins"] == 1);

    f.service.onSubmittedAuction(submission());

    REQUIRE(f.results.size() == 1);
    CHECK(f.results[0].type == MatchedWinLoss::Type::WIN);
    CHECK(f.results[0].winPrice == USD_CPM(1.0));
    REQUIRE(f.banker->calls().size() == 1);
    CHECK(f.banker->calls()[0].what == "win");
    CHECK(f.service.numSubmitted() == 0);
    CHECK(f.service.statsJson()["earlyWins"] == 0);
    CHECK(f.service.numUnmatched() == 0);
}

TEST_CASE("post auction: early win that never gets a submission is dropped")
{
    Fixture f;
    f.service.onWin(win(1.0));
    f.service.tick(now() + 1min);   // well after the 10 s early-win timeout
    CHECK(f.service.statsJson()["earlyWins"] == 0);
    CHECK(f.service.numUnmatched() == 1);

    // The submission that comes afterwards waits for its own result.
    f.service.onSubmittedAuction(submission());
    CHECK(f.results.empty());
    CHECK(f.service.numSubmitted() == 1);
}

TEST_CASE("post auction: loss timeout cancels the bid")
{
    Fixture f;
    Date timeout = now() + 50ms;
    f.service.onSubmittedAuction(submission("a1", 1, timeout));
    f.service.onSubmittedAuction(submission("a2", 1, timeout + 1h));

    f.service.tick(timeout - 1ms);
    CHECK(f.results.empty());

    f.service.tick(timeout + 1ms);
    REQUIRE(f.results.size() == 1);
    CHECK(f.results[0].type == MatchedWinLoss::Type::LOSS);
    CHECK(f.results[0].auctionId == Id("a1"));
    CHECK(f.results[0].winPrice.isZero());
    REQUIRE(f.banker->calls().size() == 1);
    CHECK(f.banker->calls()[0].what == "cancel");
    CHECK(f.banker->calls()[0].item == "a1:1:agent1");
    CHECK(f.service.numSubmitted() == 1);
    CHECK(f.service.statsJson()["losses"] == 1);

    // Ticking again does not repeat the loss.
    f.service.tick(timeout + 2ms);
    CHECK(f.results.size() == 1);
}

TEST_CASE("post auction: explicit loss cancels the bid at once")
{
    Fixture f;
    f.service.onSubmittedAuction(submission());
    f.service.onEvent(event(PostAuctionEventType::LOSS));

    REQUIRE(f.results.size() == 1);
    CHECK(f.results[0].type == MatchedWinLoss::Type::LOSS);
    REQUIRE(f.banker->calls().size() == 1);
    CHECK(f.banker->calls()[0].what == "cancel");

    // A repeated loss is ignored; a loss for an unknown bid is unmatched.
    f.service.onEvent(event(PostAuctionEventType::LOSS));
    CHECK(f.results.size() == 1);
    CHECK(f.service.numUnmatched() == 0);
    f.service.onEvent(event(PostAuctionEventType::LOSS, "nobody"));
    CHECK(f.service.numUnmatched() == 1);
}

TEST_CASE("post auction: late win after the loss timeout forces the win")
{
    Fixture f;
    Date timeout = now() + 10ms;
    f.service.onSubmittedAuction(submission("a1", 1, timeout));
    f.service.tick(timeout + 1ms);
    REQUIRE(f.results.size() == 1);
    REQUIRE(f.results[0].type == MatchedWinLoss::Type::LOSS);

    f.service.onWin(win(2.0));

    REQUIRE(f.results.size() == 2);
    CHECK(f.results[1].type == MatchedWinLoss::Type::LATE_WIN);
    CHECK(f.results[1].winPrice == USD_CPM(2.0));
    REQUIRE(f.banker->calls().size() == 2);
    CHECK(f.banker->calls()[0].what == "cancel");
    CHECK(f.banker->calls()[1].what == "forceWin");
    CHECK(f.banker->calls()[1].paid == USD_CPM(2.0));
    CHECK(f.service.statsJson()["lateWins"] == 1);

    // The late win counts as a win: campaign events match.
    f.service.onCampaignEvent(campaignEvent(CampaignEventType::IMPRESSION));
    CHECK(f.campaign.size() == 1);
}

TEST_CASE("post auction: duplicate win is ignored")
{
    Fixture f;
    f.service.onSubmittedAuction(submission());
    f.service.onWin(win(1.0));
    f.service.onWin(win(1.0));
    f.service.onWin(win(3.0));

    CHECK(f.results.size() == 1);
    CHECK(f.banker->calls().size() == 1);
    CHECK(f.service.statsJson()["wins"] == 1);
    CHECK(f.service.statsJson()["duplicateWins"] == 2);
    CHECK(f.service.numUnmatched() == 0);
}

TEST_CASE("post auction: duplicate early win is ignored")
{
    Fixture f;
    f.service.onWin(win(1.0));
    f.service.onWin(win(9.0));
    f.service.onSubmittedAuction(submission());
    REQUIRE(f.results.size() == 1);
    CHECK(f.results[0].winPrice == USD_CPM(1.0));
    CHECK(f.service.statsJson()["duplicateWins"] == 1);
}

TEST_CASE("post auction: impressions and clicks match a won bid")
{
    Fixture f;
    f.service.onSubmittedAuction(submission());

    // Not won yet: nothing to match.
    f.service.onCampaignEvent(campaignEvent(CampaignEventType::IMPRESSION));
    CHECK(f.campaign.empty());
    CHECK(f.service.numUnmatched() == 1);

    f.service.onWin(win(1.0));
    f.service.onEvent(campaignEvent(CampaignEventType::IMPRESSION));
    f.service.onEvent(campaignEvent(CampaignEventType::CLICK));
    f.service.onEvent(campaignEvent(CampaignEventType::CLICK, "unknown"));

    REQUIRE(f.campaign.size() == 2);
    CHECK(f.campaign[0].event == CampaignEventType::IMPRESSION);
    CHECK(f.campaign[1].event == CampaignEventType::CLICK);
    CHECK(f.campaign[0].auctionId == Id("a1"));
    CHECK(f.campaign[0].bid.agent == "agent1");
    CHECK(f.service.numUnmatched() == 2);
    CHECK(f.service.statsJson()["campaignEvents"] == 2);
}

TEST_CASE("post auction: finished bids are forgotten after their window")
{
    PostAuctionService::Options options;
    options.finishedWindow = 10ms;
    Fixture f(options);
    f.service.onSubmittedAuction(submission());
    f.service.onWin(win(1.0));
    CHECK(f.service.statsJson()["finished"] == 1);

    f.service.tick(now() + 1s);
    CHECK(f.service.statsJson()["finished"] == 0);
    f.service.onCampaignEvent(campaignEvent(CampaignEventType::CLICK));
    CHECK(f.campaign.empty());
}

TEST_CASE("post auction: test bids are never charged")
{
    Fixture f;
    SubmittedAuctionEvent e = submission();
    e.bid.test = true;
    f.service.onSubmittedAuction(e);
    f.service.onWin(win(1.0));
    REQUIRE(f.banker->calls().size() == 1);
    CHECK(f.banker->calls()[0].what == "cancel");
    CHECK(f.results.size() == 1);
}

TEST_CASE("post auction: spots of one auction are independent")
{
    Fixture f;
    f.service.onSubmittedAuction(submission("a1", 1));
    f.service.onSubmittedAuction(submission("a1", 2));
    f.service.onWin(win(1.0, "a1", 2));
    REQUIRE(f.results.size() == 1);
    CHECK(f.results[0].adSpotId == Id(uint64_t(2)));
    CHECK(f.service.numSubmitted() == 1);
}

TEST_CASE("post auction: a throwing banker is counted, not propagated")
{
    struct ThrowingBanker : NullBanker {
        void winBid(const AccountKey&, const std::string&, const Amount&) override
        {
            throw std::invalid_argument("boom");
        }
    };
    PostAuctionService service(std::make_shared<ThrowingBanker>());
    int wins = 0;
    service.setWinLossHandler([&](const MatchedWinLoss&) { ++wins; });
    service.onSubmittedAuction(submission());
    CHECK_NOTHROW(service.onWin(win(1.0)));
    CHECK(wins == 1);
    CHECK(service.statsJson()["bankerErrors"] == 1);
}

TEST_CASE("post auction: timer thread turns timeouts into losses")
{
    PostAuctionService::Options options;
    options.tickInterval = 5ms;
    auto banker = std::make_shared<RecordingBanker>();
    PostAuctionService service(banker, options);
    std::atomic<int> losses{0};
    service.setWinLossHandler([&](const MatchedWinLoss& r) {
        if (r.type == MatchedWinLoss::Type::LOSS) ++losses;
    });
    service.start();
    service.start();   // harmless
    service.onSubmittedAuction(submission("a1", 1, now() + 20ms));

    for (int i = 0; i < 200 && losses == 0; ++i) std::this_thread::sleep_for(10ms);
    service.stop();
    service.stop();   // harmless
    CHECK(losses == 1);
    CHECK(service.numSubmitted() == 0);
}

TEST_CASE("post auction: concurrent submissions and wins")
{
    Fixture f;
    std::mutex mutex;
    std::vector<MatchedWinLoss> seen;
    f.service.setWinLossHandler([&](const MatchedWinLoss& r) {
        std::lock_guard<std::mutex> lock(mutex);
        seen.push_back(r);
    });

    constexpr int n = 200;
    std::thread submitter([&] {
        for (int i = 0; i < n; ++i)
            f.service.onSubmittedAuction(submission("c" + std::to_string(i)));
    });
    std::thread winner([&] {
        for (int i = 0; i < n; ++i) f.service.onWin(win(1.0, "c" + std::to_string(i)));
    });
    submitter.join();
    winner.join();

    CHECK(seen.size() == size_t(n));
    CHECK(f.service.numSubmitted() == 0);
    CHECK(f.banker->calls().size() == size_t(n));
}

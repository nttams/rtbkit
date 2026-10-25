#include "adserver/adserver.h"

#include "fixtures.h"

#include <httplib.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <mutex>
#include <vector>

using namespace RTBKIT;
using namespace RTBKIT::Test;
using namespace std::chrono_literals;

namespace {

/** Banker that remembers what was won. */
class RecordingBanker : public NullBanker {
public:
    void winBid(const AccountKey&, const std::string& item, const Amount& paid) override
    {
        std::lock_guard<std::mutex> lock(mutex);
        wins.emplace_back(item, paid);
    }
    std::mutex mutex;
    std::vector<std::pair<std::string, Amount>> wins;
};

struct Fixture {
    std::shared_ptr<RecordingBanker> banker = std::make_shared<RecordingBanker>();
    PostAuctionService postAuction{banker};
    AdServer adServer{postAuction};
    std::mutex mutex;
    std::vector<MatchedWinLoss> results;
    std::vector<MatchedCampaignEvent> campaign;

    Fixture()
    {
        postAuction.setWinLossHandler([this](const MatchedWinLoss& r) {
            std::lock_guard<std::mutex> lock(mutex);
            results.push_back(r);
        });
        postAuction.setCampaignEventHandler([this](const MatchedCampaignEvent& e) {
            std::lock_guard<std::mutex> lock(mutex);
            campaign.push_back(e);
        });
        adServer.configure({{"port", 0}, {"threads", 2}});
        adServer.start();
    }

    httplib::Client client() { return httplib::Client("127.0.0.1", adServer.port()); }

    void submit(const std::string& auction = "a1", uint64_t spot = 1)
    {
        SubmittedAuctionEvent e;
        e.auctionId = Id(auction);
        e.adSpotId = Id(spot);
        e.lossTimeout = std::chrono::system_clock::now() + 1h;
        e.bid = sampleResponse(0, 2.0);
        postAuction.onSubmittedAuction(e);
    }
};

int status(const httplib::Result& result)
{
    REQUIRE(result);
    return result->status;
}

} // namespace

TEST_CASE("adserver: ping and stats")
{
    Fixture f;
    auto c = f.client();
    auto ping = c.Get("/ping");
    REQUIRE(ping);
    CHECK(ping->status == 200);
    CHECK(ping->body == "pong");

    auto stats = c.Get("/stats");
    REQUIRE(stats);
    CHECK(stats->status == 200);
    auto json = nlohmann::json::parse(stats->body);
    CHECK(json["wins"] == 0);
    CHECK(json.contains("unmatched"));
}

TEST_CASE("adserver: win notice reaches the post auction service")
{
    Fixture f;
    f.submit();
    auto c = f.client();
    // price is CPM: 1.5 CPM == 1500 micro USD per impression
    CHECK(status(c.Get("/win?auctionId=a1&adSpotId=1&price=1.5")) == 200);

    REQUIRE(f.results.size() == 1);
    CHECK(f.results[0].type == MatchedWinLoss::Type::WIN);
    CHECK(f.results[0].winPrice == USD_CPM(1.5));
    CHECK(f.results[0].winPrice.value == 1500);
    REQUIRE(f.banker->wins.size() == 1);
    CHECK(f.banker->wins[0].first == bidItemId(Id("a1"), Id(uint64_t(1)), "agent1"));
    CHECK(f.banker->wins[0].second == USD_CPM(1.5));

    auto stats = nlohmann::json::parse(c.Get("/stats")->body);
    CHECK(stats["wins"] == 1);
}

TEST_CASE("adserver: a huge win price charges only the bid price")
{
    Fixture f;
    f.submit();   // bid price 2.0 CPM
    auto c = f.client();
    CHECK(status(c.Get("/win?auctionId=a1&adSpotId=1&price=1000000000")) == 200);

    REQUIRE(f.banker->wins.size() == 1);
    CHECK(f.banker->wins[0].second == USD_CPM(2.0));
    REQUIRE(f.results.size() == 1);
    CHECK(f.results[0].winPrice == USD_CPM(2.0));
    auto stats = nlohmann::json::parse(c.Get("/stats")->body);
    CHECK(stats["clamped"] == 1);
}

TEST_CASE("adserver: win notice before the submission is matched later")
{
    Fixture f;
    auto c = f.client();
    CHECK(status(c.Get("/win?auctionId=a1&adSpotId=1&price=0.25&currency=USD")) == 200);
    CHECK(f.results.empty());
    f.submit();
    REQUIRE(f.results.size() == 1);
    CHECK(f.results[0].winPrice == USD_CPM(0.25));
}

TEST_CASE("adserver: impression and click are matched to a won bid")
{
    Fixture f;
    f.submit();
    auto c = f.client();
    CHECK(status(c.Get("/win?auctionId=a1&adSpotId=1&price=1")) == 200);
    CHECK(status(c.Get("/impression?auctionId=a1&adSpotId=1")) == 200);
    CHECK(status(c.Get("/click?auctionId=a1&adSpotId=1")) == 200);
    // valid but unknown: still 200, counted as unmatched
    CHECK(status(c.Get("/click?auctionId=zzz&adSpotId=1")) == 200);

    REQUIRE(f.campaign.size() == 2);
    CHECK(f.campaign[0].event == CampaignEventType::IMPRESSION);
    CHECK(f.campaign[1].event == CampaignEventType::CLICK);
    CHECK(f.postAuction.numUnmatched() == 1);
}

TEST_CASE("adserver: bad or missing parameters give 400 and no event")
{
    Fixture f;
    f.submit();
    auto c = f.client();
    CHECK(status(c.Get("/win")) == 400);
    CHECK(status(c.Get("/win?auctionId=a1&adSpotId=1")) == 400);
    CHECK(status(c.Get("/win?auctionId=a1&price=1")) == 400);
    CHECK(status(c.Get("/win?adSpotId=1&price=1")) == 400);
    CHECK(status(c.Get("/win?auctionId=&adSpotId=1&price=1")) == 400);
    CHECK(status(c.Get("/win?auctionId=a1&adSpotId=1&price=abc")) == 400);
    CHECK(status(c.Get("/win?auctionId=a1&adSpotId=1&price=1.5x")) == 400);
    CHECK(status(c.Get("/win?auctionId=a1&adSpotId=1&price=-1")) == 400);
    CHECK(status(c.Get("/win?auctionId=a1&adSpotId=1&price=nan")) == 400);
    CHECK(status(c.Get("/win?auctionId=a1&adSpotId=1&price=1&currency=XXX")) == 400);
    CHECK(status(c.Get("/impression")) == 400);
    CHECK(status(c.Get("/impression?auctionId=a1")) == 400);
    CHECK(status(c.Get("/click?adSpotId=1")) == 400);
    CHECK(status(c.Get("/nothing")) == 404);

    CHECK(f.results.empty());
    CHECK(f.campaign.empty());
    CHECK(f.postAuction.numSubmitted() == 1);
}

TEST_CASE("adserver: configure validates and stop is idempotent")
{
    PostAuctionService postAuction(std::make_shared<NullBanker>());
    AdServer adServer(postAuction);
    CHECK_THROWS_AS(adServer.configure(nlohmann::json::array()), std::runtime_error);
    CHECK_THROWS_AS(adServer.configure({{"port", "x"}}), std::runtime_error);
    CHECK_THROWS_AS(adServer.configure({{"port", 70000}}), std::runtime_error);
    CHECK_THROWS_AS(adServer.configure({{"threads", 0}}), std::runtime_error);
    CHECK(adServer.port() == 12360);   // the default

    adServer.configure({{"port", 0}});
    adServer.start();
    CHECK(adServer.port() > 0);
    adServer.stop();
    adServer.stop();
}

TEST_CASE("adserver: binding a port that is taken throws")
{
    Fixture f;
    PostAuctionService other(std::make_shared<NullBanker>());
    AdServer second(other);
    second.configure({{"port", f.adServer.port()}});
    CHECK_THROWS_AS(second.start(), std::runtime_error);
}

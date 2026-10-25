// BidRequest <-> native JSON round trips.

#include "common/bid_request_json.h"

#include "fixtures.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace RTBKIT;
using namespace RTBKIT::Test;
using nlohmann::json;

namespace {

void checkRoundTrip(const BidRequest& request)
{
    json j = toJson(request);
    BidRequest back = bidRequestFromJson(j);
    CHECK(toJson(back) == j);

    // And through text, as on the wire.
    BidRequest viaText = bidRequestFromJson(json::parse(j.dump()));
    CHECK(toJson(viaText) == j);

    CHECK(back.auctionId == request.auctionId);
    CHECK(back.imp.size() == request.imp.size());
    CHECK(back.exchange == request.exchange);
    CHECK(back.timeAvailableMs == request.timeAvailableMs);
}

} // namespace

TEST_CASE("bid request json: sample request")
{
    auto request = sampleBidRequest();
    request->userIds.add(Id("xid"), ID_EXCHANGE);
    request->userIds.add(Id("pid"), ID_PROVIDER);
    request->segments.add("audience", 12);
    request->segments.add("audience", "sports");

    checkRoundTrip(*request);

    BidRequest back = bidRequestFromJson(toJson(*request));
    CHECK(back.userIds.exchangeId == Id("xid"));
    CHECK(back.userIds.providerId == Id("pid"));
    CHECK(back.segments.get("audience").ints == std::vector<int>{12});
    CHECK(back.imp[0].reservePrice == USD_CPM(0.5));
    CHECK(back.imp[1].reservePrice.isZero());
    CHECK(back.imp[0].formats[0] == Format(300, 250));
    CHECK(back.bidCurrency == std::vector<CurrencyCode>{CurrencyCode::USD});

    // Operators for nlohmann.
    json viaOperator = *request;
    CHECK(viaOperator == toJson(*request));
    CHECK(viaOperator.get<BidRequest>().auctionId == request->auctionId);
}

TEST_CASE("bid request json: parsed OpenRTB samples")
{
    for (const char* file : {"openrtb_banner.json", "openrtb_video.json",
                             "openrtb_mobile.json", "openrtb_wseat_req.json",
                             "openrtb2_req.json", "openrtb_2_2_req_imp.json"}) {
        DYNAMIC_SECTION(file) {
            OpenRtbOptions options;
            options.exchange = "test-exchange";
            BidRequest request = parseOpenRtbBidRequestText(readTestFile(file), options);
            checkRoundTrip(request);
        }
    }
}

TEST_CASE("bid request json: optional members and NaN")
{
    auto request = sampleBidRequest();
    request->timestamp = dateFromMs(1'700'000'000'123);
    request->isTest = true;
    request->auctionType = AuctionType::FirstPrice;
    request->ext = json{{"a", 1}};

    Device device;
    device.ip = "1.2.3.4";
    device.geo = Geo{};   // lat / lon are NaN
    request->device = device;

    User user;
    user.id = Id("u1");
    user.yearOfBirth = 1980;
    user.data.push_back(UserData{Id("d"), "dp", {"1"}, {"one"}});
    request->user = user;

    Site site;
    site.id = Id("s");
    site.domain = "example.com";
    site.publisher.name = "pub";
    request->site = site;

    request->imp[0].video = Video{640, 480, 5, 30, 1, {"video/mp4"}};
    request->imp[0].pmp = Pmp{1, {Deal{Id("deal"), USD_CPM(2), 1, {"seat"}, {}}}};

    checkRoundTrip(*request);

    BidRequest back = bidRequestFromJson(toJson(*request));
    CHECK(dateToMs(back.timestamp) == 1'700'000'000'123);
    CHECK(back.isTest);
    CHECK(back.auctionType == AuctionType::FirstPrice);
    REQUIRE(back.device->geo);
    CHECK(std::isnan(back.device->geo->lat));
    CHECK(back.user->data[0].segmentNames == std::vector<std::string>{"one"});
    CHECK(back.imp[0].video->maxDuration == 30);
    CHECK(back.imp[0].pmp->deals[0].bidfloor == USD_CPM(2));
    CHECK_FALSE(back.app.has_value());
}

TEST_CASE("bid request json: malformed input")
{
    CHECK_THROWS_AS(bidRequestFromJson(json::array()), std::runtime_error);
    CHECK_THROWS_AS(bidRequestFromJson(json{{"imp", 5}}), std::runtime_error);
    CHECK_THROWS_AS(bidRequestFromJson(json{{"bidCurrency", json::array({"XXX"})}}), std::runtime_error);
}

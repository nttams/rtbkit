// OpenRTB 2.x parsing. The sample requests in data/ come from the original
// RTBKit bid request plugin tests.

#include "common/openrtb_parser.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <fstream>
#include <sstream>

using namespace RTBKIT;
using namespace std::chrono_literals;

namespace {

std::string readFile(const std::string& name)
{
    std::ifstream in(std::string(RTB_TEST_DATA_DIR) + "/" + name);
    REQUIRE(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

BidRequest parseFile(const std::string& name, const std::string& exchange = "test-exchange")
{
    OpenRtbOptions options;
    options.exchange = exchange;
    return parseOpenRtbBidRequestText(readFile(name), options);
}

BidRequest parse(const std::string& text)
{
    return parseOpenRtbBidRequestText(text);
}

bool hasSegment(const BidRequest& br, const std::string& source, const std::string& value)
{
    bool found = false;
    br.segments.get(source).forEach([&](int, const std::string& s) {
        if (s == value) found = true;
    });
    return found;
}

} // namespace

TEST_CASE("openrtb: desktop banner request")
{
    BidRequest br = parseFile("openrtb_banner.json");

    CHECK(br.auctionId == Id("1234534625254"));
    CHECK(br.auctionType == AuctionType::SecondPrice);
    CHECK(br.timeAvailableMs == 120);
    CHECK(br.exchange == "test-exchange");
    CHECK(br.timestamp != Date());

    REQUIRE(br.imp.size() == 1);
    const AdSpot& spot = br.imp[0];
    CHECK(spot.id == Id("1"));
    CHECK(spot.isBanner);
    REQUIRE(spot.formats.size() == 1);
    CHECK(spot.formats[0] == Format(300, 250));
    CHECK(spot.position == AD_POS_ABOVE);
    CHECK(spot.reservePrice.isZero());

    REQUIRE(br.site.has_value());
    CHECK(!br.app.has_value());
    CHECK(br.site->domain == "siteabcd.com");
    CHECK(br.site->publisher.name == "Publisher A");
    CHECK(br.url.toString() == "http://siteabcd.com/page.htm");
    CHECK(br.url.host() == "siteabcd.com");

    CHECK(br.ipAddress == "64.124.253.1");
    CHECK(br.userAgent.starts_with("Mozilla/5.0 (Macintosh"));
    CHECK(br.language.empty());

    CHECK(br.userIds.exchangeId == Id("45asdf987656789adfad4678rew656789"));
    CHECK(br.userIds.providerId == Id("5df678asd8987656asdf78987654"));
    CHECK(br.userIds.size() == 2);

    CHECK(hasSegment(br, "iab-categories", "IAB2-1"));
    CHECK(hasSegment(br, "iab-categories", "IAB2-2"));

    REQUIRE(br.bidCurrency.size() == 1);
    CHECK(br.bidCurrency[0] == CurrencyCode::USD);
}

TEST_CASE("openrtb: mobile app request")
{
    BidRequest br = parseFile("openrtb_mobile.json", "mobile-exchange");

    CHECK(br.exchange == "mobile-exchange");
    CHECK(!br.site.has_value());
    REQUIRE(br.app.has_value());
    CHECK(br.app->name == "App 123");

    // No bundle: the url is derived from the app id.
    CHECK(br.url.toString() == "http://123123.appid/");

    REQUIRE(br.imp.size() == 1);
    REQUIRE(br.imp[0].formats.size() == 1);
    CHECK(br.imp[0].formats[0] == Format(300, 50));

    REQUIRE(br.device.has_value());
    CHECK(br.device->make == "Apple");
    CHECK(br.device->model == "iPhone 3GS");
    CHECK(br.ipAddress == "192.168.1.8");
    CHECK(br.location.countryCode == "USA");
    CHECK(br.location.cityName == "US SFO");

    // Data provider segments are keyed by the provider id; ids that do not
    // fit an int stay strings.
    CHECK(hasSegment(br, "6", "12341318394918"));
    CHECK(hasSegment(br, "6", "1234131839491234"));
    CHECK(hasSegment(br, "iab-categories", "IAB2-2"));
}

TEST_CASE("openrtb: banner with floor, device geo and user")
{
    BidRequest br = parseFile("openrtb2_req.json");

    CHECK(br.auctionId == Id("-ad2c310:1347a70eba8:-8000"));
    REQUIRE(br.imp.size() == 1);
    CHECK(br.imp[0].id == Id("8345y78945239"));
    CHECK(br.imp[0].formats == FormatSet{ { Format(100, 123) } });

    // 0.1503213 CPM is 150 micro dollars per impression.
    CHECK(br.imp[0].reservePrice == Amount(CurrencyCode::USD, 150));
    CHECK(br.imp[0].reservePrice == USD_CPM(0.15));

    CHECK(br.ipAddress == "127.0.0.1");
    CHECK(br.url.empty());
    CHECK(br.location.countryCode == "US");
    CHECK(br.location.regionCode == "US-MA");
    CHECK(br.location.cityName == "Boston");
    CHECK(br.location.postalCode == "02111");

    REQUIRE(br.device.has_value());
    REQUIRE(br.device->geo.has_value());
    CHECK(br.device->geo->lat == 13.516f);
    CHECK(br.device->geo->lon == -122.625f);

    REQUIRE(br.user.has_value());
    CHECK(br.user->gender == "F");
    CHECK(br.user->yearOfBirth == 1999);

    // No buyeruid: the user id doubles as provider id.
    CHECK(br.userIds.exchangeId == Id("User_-ad2c310:1347a70eba8:-7fff"));
    CHECK(br.userIds.providerId == br.userIds.exchangeId);
}

TEST_CASE("openrtb: request with a pmp block and extra banner fields")
{
    BidRequest br = parseFile("openrtb_2_2_req_imp.json");
    REQUIRE(br.imp.size() == 1);
    REQUIRE(br.imp[0].pmp.has_value());
    CHECK(br.imp[0].pmp->privateAuction == 0);
    CHECK(br.imp[0].pmp->deals.empty());
    CHECK(br.imp[0].formats == FormatSet{ { Format(100, 123) } });
    CHECK(br.imp[0].reservePrice.value == 150);
}

TEST_CASE("openrtb: video request")
{
    BidRequest br = parseFile("openrtb_video.json");

    REQUIRE(br.imp.size() == 1);
    const AdSpot& spot = br.imp[0];
    CHECK(!spot.isBanner);
    REQUIRE(spot.video.has_value());
    CHECK(spot.video->width == 640);
    CHECK(spot.video->height == 480);
    CHECK(spot.video->minDuration == 5);
    CHECK(spot.video->maxDuration == 30);
    CHECK(spot.video->mimes.size() == 4);
    CHECK(spot.position == AD_POS_ABOVE);
    REQUIRE(spot.formats.size() == 1);
    CHECK(spot.formats[0] == Format(640, 480));

    // This old style request has no user id, only a buyeruid.
    CHECK(!br.userIds.contains(ID_EXCHANGE));
    CHECK(br.userIds.providerId == Id("545678765467876567898765678987654"));
    CHECK(br.url.toString() == "http://siteabcd.com/page.htm");
}

TEST_CASE("openrtb: wseat becomes a segment source")
{
    BidRequest br = parseFile("openrtb_wseat_req.json");

    const SegmentList& wseat = br.segments.get("openrtb-wseat");
    CHECK(wseat.ints == std::vector<int>{ 4, 34, 82 });
    CHECK(wseat.strings == std::vector<std::string>{ "A45", "%84!" });
    CHECK(br.imp[0].reservePrice == Amount(CurrencyCode::USD, 150));
}

TEST_CASE("openrtb: defaults and request level fields")
{
    OpenRtbOptions options;
    options.provider = "prov-name";
    options.now = std::chrono::system_clock::time_point(1000s);

    BidRequest br = parseOpenRtbBidRequestText(R"({
        "id": 42, "at": 1, "tmax": 80, "cur": ["EUR", "USD"],
        "bcat": ["IAB25"], "badv": ["bad.com"],
        "imp": [ { "id": "a", "bidfloor": 2.0, "bidfloorcur": "EUR",
                   "tagid": "tag", "instl": 1,
                   "banner": { "format": [ {"w": 300, "h": 250}, {"w": 320, "h": 50} ],
                               "api": [3, 5] } } ],
        "ext": { "k": "v" }
    })", options);

    CHECK(br.auctionId == Id(uint64_t(42)));
    CHECK(br.auctionType == AuctionType::FirstPrice);
    CHECK(br.timeAvailableMs == 80);
    CHECK(br.timestamp == options.now.value());
    CHECK(br.provider == "prov-name");
    CHECK(br.exchange == "prov-name");
    CHECK(br.bidCurrency == std::vector<CurrencyCode>{ CurrencyCode::EUR, CurrencyCode::USD });
    CHECK(br.blockedCategories == std::vector<std::string>{ "IAB25" });
    CHECK(br.badv == std::vector<std::string>{ "bad.com" });
    CHECK(br.restrictions.get("badv").strings == std::vector<std::string>{ "bad.com" });
    CHECK(br.ext["k"] == "v");

    const AdSpot& spot = br.imp[0];
    CHECK(spot.tagId == "tag");
    CHECK(spot.interstitial);
    CHECK(spot.formats.size() == 2);
    CHECK(spot.formats.compatible(Format(320, 50)));
    CHECK(spot.position == AD_POS_UNSPECIFIED);
    CHECK(spot.reservePrice == Amount(CurrencyCode::EUR, 2000));
    CHECK(hasSegment(br, "supportedSDKs", "MRAID"));
    CHECK(!hasSegment(br, "supportedSDKs", "ORMMA"));

    // Without user and device the provider id is zero.
    CHECK(br.userIds.providerId == Id(uint64_t(0)));
    CHECK(!br.userIds.contains(ID_EXCHANGE));

    // Without tmax / cur the documented defaults apply.
    BidRequest minimal = parse(R"({"id": "x", "imp": [ {"id": "1"} ]})");
    CHECK(minimal.timeAvailableMs == 100);
    CHECK(minimal.bidCurrency == std::vector<CurrencyCode>{ CurrencyCode::USD });
    CHECK(minimal.imp[0].formats.empty());
}

TEST_CASE("openrtb: provider id falls back to a stable ip + ua hash")
{
    const char* body = R"({"id": "x", "imp": [ {"id": "1"} ],
                           "device": {"ip": "1.2.3.4", "ua": "agent"}})";
    BidRequest a = parse(body);
    BidRequest b = parse(body);

    CHECK(!a.userIds.providerId.empty());
    CHECK(a.userIds.providerId == b.userIds.providerId);
    CHECK(a.userIds.providerId != Id(uint64_t(0)));

    BidRequest c = parse(R"({"id": "x", "imp": [ {"id": "1"} ],
                             "device": {"ip": "1.2.3.5", "ua": "agent"}})");
    CHECK(c.userIds.providerId != a.userIds.providerId);
}

TEST_CASE("openrtb: location details")
{
    BidRequest br = parse(R"({"id": "x", "imp": [ {"id": "1"} ],
        "device": { "ipv6": "::1", "language": "fr",
                    "geo": { "country": "CA", "regionfips104": "CA08",
                             "city": "Toronto", "zip": "M5V", "metro": "501" } },
        "user": { "id": "u", "geo": { "country": "US", "city": "Other" } } })");

    CHECK(br.ipAddress == "::1");
    CHECK(br.language == "fr");
    CHECK(br.location.countryCode == "CA");
    CHECK(br.location.regionCode == "CA08");
    CHECK(br.location.metro == 501);
    CHECK(br.location.cityName == "Toronto");
    CHECK(br.location.fullLocationString() == "CA:CA08:Toronto:M5V:-1:501");
}

TEST_CASE("openrtb: private marketplace deals")
{
    BidRequest br = parse(R"({"id": "x", "imp": [ {"id": "1",
        "pmp": { "private_auction": 1, "deals": [
            { "id": "DEAL1", "bidfloor": 3.0, "at": 1, "wseat": ["s1"] },
            { "id": "DEAL2" } ] } } ]})");

    REQUIRE(br.imp[0].pmp.has_value());
    const Pmp& pmp = *br.imp[0].pmp;
    CHECK(pmp.privateAuction == 1);
    REQUIRE(pmp.deals.size() == 2);
    CHECK(pmp.deals[0].id == Id("DEAL1"));
    CHECK(pmp.deals[0].bidfloor == USD_CPM(3.0));
    CHECK(pmp.deals[0].auctionType == 1);
    CHECK(pmp.deals[0].wseat == std::vector<std::string>{ "s1" });
    CHECK(pmp.deals[1].bidfloor.isZero());
}

TEST_CASE("openrtb: invalid requests are rejected")
{
    SECTION("not json") { CHECK_THROWS_AS(parse("{nope"), OpenRtbError); }
    SECTION("not an object") { CHECK_THROWS_AS(parse("[1]"), OpenRtbError); }
    SECTION("no id") { CHECK_THROWS_AS(parse(R"({"imp": [ {"id": "1"} ]})"), OpenRtbError); }
    SECTION("empty id") { CHECK_THROWS_AS(parse(R"({"id": "", "imp": [ {"id": "1"} ]})"), OpenRtbError); }
    SECTION("imp without id") { CHECK_THROWS_AS(parse(R"({"id": "x", "imp": [ {"banner": {}} ]})"), OpenRtbError); }
    SECTION("empty imp id") { CHECK_THROWS_AS(parse(R"({"id": "x", "imp": [ {"id": ""} ]})"), OpenRtbError); }
    SECTION("duplicate imp id")
    {
        CHECK_THROWS_AS(parse(R"({"id": "x", "imp": [ {"id": "1"}, {"id": "1"} ]})"), OpenRtbError);
        CHECK_THROWS_AS(parse(R"({"id": "x", "imp": [ {"id": 1}, {"id": "1"} ]})"), OpenRtbError);
    }
    SECTION("no imp") { CHECK_THROWS_AS(parse(R"({"id": "x"})"), OpenRtbError); }
    SECTION("empty imp") { CHECK_THROWS_AS(parse(R"({"id": "x", "imp": []})"), OpenRtbError); }
    SECTION("site and app")
    {
        CHECK_THROWS_AS(parse(R"({"id": "x", "imp": [ {"id": "1"} ],
                                  "site": {"id": "s"}, "app": {"id": "a"}})"),
                        OpenRtbError);
    }
    SECTION("wrong field type")
    {
        CHECK_THROWS_AS(parse(R"({"id": "x", "tmax": "fast", "imp": [ {"id": "1"} ]})"),
                        OpenRtbError);
        CHECK_THROWS_AS(parse(R"({"id": "x", "imp": [ {"id": "1", "banner": {"w": "a", "h": 1}} ]})"),
                        OpenRtbError);
    }
    SECTION("unsupported currency")
    {
        CHECK_THROWS_AS(parse(R"({"id": "x", "cur": ["XXX"], "imp": [ {"id": "1"} ]})"),
                        OpenRtbError);
        CHECK_THROWS_AS(parse(R"({"id": "x", "imp": [ {"id": "1", "bidfloor": 1, "bidfloorcur": "XXX"} ]})"),
                        OpenRtbError);
    }
    SECTION("video durations")
    {
        CHECK_THROWS_AS(parse(R"({"id": "x", "imp": [ {"id": "1",
            "video": {"minduration": 30, "maxduration": 5}} ]})"), OpenRtbError);
    }
}

// Mock exchange helpers: request generator, url and statistics functions.

#include "exchange/mock_exchange_support.h"

#include "common/openrtb_parser.h"

#include <catch2/catch_test_macros.hpp>

#include <set>

using namespace RTBKIT;
using nlohmann::json;

TEST_CASE("mock exchange: generated requests parse as OpenRTB")
{
    std::mt19937_64 rng(42);
    std::set<std::string> ids;
    std::set<std::string> hosts, languages, countries;
    std::set<std::pair<int, int>> sizes;
    int withFloor = 0, withoutFloor = 0, twoImps = 0;

    for (int i = 0; i < 500; ++i) {
        json j = generateOpenRtbRequest(rng);
        BidRequest r = parseOpenRtbBidRequest(j);

        CHECK(ids.insert(r.auctionId.toString()).second);
        CHECK(r.timeAvailableMs == 100);
        REQUIRE(!r.imp.empty());
        REQUIRE(r.imp.size() <= 2);
        twoImps += r.imp.size() == 2;
        for (const auto& spot : r.imp) {
            CHECK(spot.isBanner);
            REQUIRE_FALSE(spot.formats.empty());
            for (const Format& f : spot.formats) sizes.insert({f.width, f.height});
            if (spot.reservePrice > Amount()) {
                ++withFloor;
                CHECK(spot.reservePrice <= USD_CPM(1.0));
            }
            else ++withoutFloor;
        }
        REQUIRE(r.site);
        hosts.insert(r.url.host());
        CHECK_FALSE(r.url.host().empty());
        REQUIRE(r.device);
        languages.insert(r.device->language);
        CHECK_FALSE(r.device->ip.empty());
        REQUIRE(r.device->geo);
        countries.insert(r.device->geo->country);
        REQUIRE(r.user);
        CHECK_FALSE(r.user->id.empty());
        CHECK_FALSE(r.user->buyerUid.empty());
        CHECK(r.bidCurrency == std::vector<CurrencyCode>{CurrencyCode::USD});
    }

    CHECK(sizes == std::set<std::pair<int, int>>{{300, 250}, {728, 90}});
    CHECK(hosts.size() > 3);
    CHECK(languages.size() > 2);
    CHECK(countries.size() > 2);
    CHECK(withFloor > 0);
    CHECK(withoutFloor > 0);
    CHECK(twoImps > 0);
}

TEST_CASE("mock exchange: generator options and determinism")
{
    MockRequestOptions options;
    options.tmaxMs = 250;
    options.noFloorProbability = 0;
    options.maxFloorCpm = 0.5;

    std::mt19937_64 a(7), b(7);
    json x = generateOpenRtbRequest(a, options);
    json y = generateOpenRtbRequest(b, options);
    // The id carries a global counter, everything else follows the seed.
    x.erase("id");
    y.erase("id");
    CHECK(x == y);

    BidRequest r = parseOpenRtbBidRequest(generateOpenRtbRequest(a, options));
    CHECK(r.timeAvailableMs == 250);
    for (const auto& spot : r.imp) {
        CHECK(spot.reservePrice > Amount());
        CHECK(spot.reservePrice <= USD_CPM(0.5));
    }
}

TEST_CASE("mock exchange: splitUrl")
{
    using P = std::pair<std::string, std::string>;
    CHECK(splitUrl("http://127.0.0.1:12360/win?a=1&b=2") == P{"http://127.0.0.1:12360", "/win?a=1&b=2"});
    CHECK(splitUrl("http://example.com") == P{"http://example.com", "/"});
    CHECK(splitUrl("https://example.com/") == P{"https://example.com", "/"});
    CHECK_THROWS_AS(splitUrl("example.com/x"), std::invalid_argument);
    CHECK_THROWS_AS(splitUrl("http:///x"), std::invalid_argument);
    CHECK_THROWS_AS(splitUrl(""), std::invalid_argument);
}

TEST_CASE("mock exchange: win notice price macro")
{
    CHECK(replaceAuctionPrice("http://h/win?price=${AUCTION_PRICE}", 1.5) == "http://h/win?price=1.5");
    CHECK(replaceAuctionPrice("http://h/win?price=${AUCTION_PRICE}", 2) == "http://h/win?price=2.0");
    CHECK(replaceAuctionPrice("http://h/win?price=${AUCTION_PRICE}", 0.123456) == "http://h/win?price=0.123456");
    CHECK(replaceAuctionPrice("a${AUCTION_PRICE}b${AUCTION_PRICE}", 3.25) == "a3.25b3.25");
    CHECK(replaceAuctionPrice("http://h/win", 3.25) == "http://h/win");
}

TEST_CASE("mock exchange: query parameters")
{
    std::string url = "http://h/win?auctionId=a%20b%26c&adSpotId=2&price=1.5";
    CHECK(queryParam(url, "auctionId") == "a b&c");
    CHECK(queryParam(url, "adSpotId") == "2");
    CHECK(queryParam(url, "price") == "1.5");
    CHECK(queryParam(url, "missing").empty());
    CHECK(queryParam("http://h/win", "price").empty());
    CHECK(queryParam("http://h/win?flag", "flag").empty());
    CHECK(queryParam(url, "auction").empty());

    std::string odd = "x/ y&z=é";
    CHECK(queryParam("http://h/?k=" + urlEncode(odd), "k") == odd);
    CHECK(urlEncode("abc-_.~123") == "abc-_.~123");
    CHECK(urlEncode("a b") == "a%20b");
}

TEST_CASE("mock exchange: percentile")
{
    std::vector<double> empty;
    CHECK(percentile(empty, 0.5) == 0);

    std::vector<double> v;
    for (int i = 100; i >= 1; --i) v.push_back(i);
    CHECK(percentile(v, 0.50) == 50);
    CHECK(percentile(v, 0.99) == 99);
    CHECK(percentile(v, 1.0) == 100);
    CHECK(percentile(v, 0.0) == 1);

    std::vector<double> one{4.5};
    CHECK(percentile(one, 0.99) == 4.5);
}

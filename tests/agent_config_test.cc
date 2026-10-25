// AgentConfig / Creative JSON parsing in the original RTBKit format.

#include "agent/agent_config.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <fstream>
#include <sstream>

using namespace RTBKIT;
using nlohmann::json;

namespace {

AgentConfig parse(const std::string& text) { return AgentConfig::parse(text); }

// Smallest valid config, with extra members spliced in.
AgentConfig parseWith(const std::string& members)
{
    return parse(R"({"account": ["a", "b"], "creatives": [{"id": 0, "width": 1, "height": 1}])"
                 + (members.empty() ? "" : ", " + members) + "}");
}

} // namespace

TEST_CASE("agent config: minimal config as in examples/http_config.json")
{
    // This is the "Config" member of rtbkit/examples/http_config.json.
    AgentConfig c = parse(R"({
        "account": ["hello", "world0"],
        "externalId": 123,
        "bidderInterface": "iface.http",
        "bidProbability": 0.5,
        "creatives": [
            { "id": 1, "width": 300, "height": 250 },
            { "id": 2, "width": 300, "height": 250 }
        ]
    })");

    CHECK(c.account == AccountKey{ "hello", "world0" });
    CHECK(c.externalId == 123);
    CHECK(c.bidderInterface == "iface.http");
    CHECK(c.bidProbability == 0.5f);
    REQUIRE(c.creatives.size() == 2);
    CHECK(c.creatives[0].id == 1);
    CHECK(c.creatives[0].format == Format(300, 250));
    CHECK(c.creatives[1].id == 2);

    // Defaults of the original.
    CHECK(c.minTimeAvailableMs == 5.0f);
    CHECK(c.maxInFlight == 100);
    CHECK(!c.test);
    CHECK(!c.external);
    CHECK(c.hourOfWeekFilter.isDefault());
    CHECK(c.userPartition.empty());
    CHECK(c.bidControlType == BC_RELAY);
    CHECK(c.winFormat == BRF_FULL);
    CHECK(c.lossFormat == BRF_LIGHTWEIGHT);
    CHECK(!c.hasBlacklist());
    CHECK(c.augmentations.empty());
}

TEST_CASE("agent config: bidding agent example with augmentations")
{
    // Taken from rtbkit/examples/nodeagents/nodebidagent-config.js.
    AgentConfig c = parse(R"({
        "account": ["hello","world"],
        "bidProbability": 1.0,
        "creatives": [
            { "format":{"width":720, "height":90}, "id":0, "name":"LeaderBoard", "tagId":0 },
            { "format":{"width":160,"height":600}, "id":1, "name":"LeaderBoard", "tagId":1 },
            { "format":"300x250", "id":2, "name":"BigBox", "tagId":2 }
        ],
        "augmentations":{
            "frequency-cap-ex":{
                "required":true,
                "config":42,
                "filters":{"include":["pass-frequency-cap-ex"]}
            }
        },
        "maxInFlight": 50
    })");

    REQUIRE(c.creatives.size() == 3);
    CHECK(c.creatives[0].format == Format(720, 90));
    CHECK(c.creatives[0].name == "LeaderBoard");
    CHECK(c.creatives[2].format == Format(300, 250));
    CHECK(c.creatives[2].extensions["tagId"] == 2);   // unknown keys are kept
    CHECK(c.maxInFlight == 50);

    REQUIRE(c.augmentations.size() == 1);
    CHECK(c.augmentations[0].name == "frequency-cap-ex");
    CHECK(c.augmentations[0].required);
    CHECK(c.augmentations[0].config == 42);
    CHECK(c.augmentations[0].filters.include
          == std::vector<std::string>{ "pass-frequency-cap-ex" });
}

TEST_CASE("agent config: every field")
{
    AgentConfig c = parse(R"({
        "account": "campaign:strategy",
        "name": "my-agent",
        "externalId": 7,
        "external": true,
        "test": true,
        "roundRobin": { "group": "g", "weight": 3 },
        "bidProbability": 0.25,
        "minTimeAvailableMs": 12.5,
        "maxInFlight": 9,
        "bidderInterface": "zmq",
        "requiredIds": ["xchg", "prov"],
        "urlFilter": { "include": ["^http://news"], "exclude": ["porn"] },
        "hostFilter": { "include": ["example.com"] },
        "languageFilter": { "include": ["en", "fr"] },
        "locationFilter": { "include": ["^US:"] },
        "exchangeFilter": { "exclude": ["bad-exchange"] },
        "latLongDevFilter": [ { "lat": 37.5, "long": -122.4, "radius": 10 } ],
        "foldPositionFilter": { "include": [1, "BELOW"], "exclude": [7] },
        "segmentFilter": {
            "audience": { "include": ["b", 3, 1], "exclude": [9],
                          "excludeIfNotPresent": true,
                          "applyToExchanges": { "include": ["demo"] } }
        },
        "tagFilter": { "include": ["t"] },
        "userPartition": { "hashOn": "ipua", "modulus": 4,
                           "includeRanges": [[0, 1], [2, 4]] },
        "blacklist": { "type": "USER_SITE", "scope": "agent", "time": 30 },
        "visits": { "channels": ["c1", 2], "includeUnmatched": true },
        "bidControl": { "type": "relay_fixed", "fixedBidCpmInMicros": 2500 },
        "providerConfig": { "demo": { "seat": 1 } },
        "winFormat": "lightweight", "lossFormat": "none", "errorFormat": "full",
        "ext": { "k": 1 },
        "somethingNew": true,
        "creatives": [ { "id": 5, "width": 10, "height": 20 } ]
    })");

    CHECK(c.account.toString() == "campaign:strategy");
    CHECK(c.name == "my-agent");
    CHECK(c.externalId == 7);
    CHECK(c.external);
    CHECK(c.test);
    CHECK(c.roundRobinGroup == "g");
    CHECK(c.roundRobinWeight == 3);
    CHECK(c.bidProbability == 0.25f);
    CHECK(c.minTimeAvailableMs == 12.5f);
    CHECK(c.maxInFlight == 9);
    CHECK(c.requiredIds == std::vector<std::string>{ "xchg", "prov" });

    CHECK(c.urlFilter.include.size() == 1);
    CHECK(matches(c.urlFilter.include[0], std::string("http://news.example.com")));
    CHECK(c.urlFilter.exclude[0].str == "porn");
    CHECK(c.hostFilter.include[0].str == "example.com");
    CHECK(c.languageFilter.include.size() == 2);
    CHECK(c.locationFilter.include[0].str == "^US:");
    CHECK(c.exchangeFilter.exclude == std::vector<std::string>{ "bad-exchange" });

    REQUIRE(c.latLongDevFilter.latlonrads.size() == 1);
    CHECK(c.latLongDevFilter.latlonrads[0].lat == 37.5f);
    CHECK(c.latLongDevFilter.latlonrads[0].lon == -122.4f);
    CHECK(c.latLongDevFilter.latlonrads[0].radius == 10.0f);

    CHECK(c.foldPositionFilter.include == std::vector<int>{ 1, 3 });
    CHECK(c.foldPositionFilter.exclude == std::vector<int>{ 7 });

    REQUIRE(c.segments.count("audience") == 1);
    const auto& seg = c.segments.at("audience");
    CHECK(seg.excludeIfNotPresent);
    CHECK(seg.include.ints == std::vector<int>{ 1, 3 });      // sorted
    CHECK(seg.include.strings == std::vector<std::string>{ "b" });
    CHECK(seg.exclude.ints == std::vector<int>{ 9 });
    CHECK(seg.applyToExchanges.include == std::vector<std::string>{ "demo" });
    CHECK(c.tagFilter.include.strings == std::vector<std::string>{ "t" });

    CHECK(c.userPartition.hashOn == UserPartition::IPUA);
    CHECK(c.userPartition.modulus == 4);
    REQUIRE(c.userPartition.includeRanges.size() == 2);
    CHECK(c.userPartition.includeRanges[1].first == 2);
    CHECK(c.userPartition.includeRanges[1].last == 4);
    CHECK(c.userPartition.includeRanges[1].in(3));
    CHECK(!c.userPartition.includeRanges[1].in(4));

    CHECK(c.blacklistType == BL_USER_SITE);
    CHECK(c.blacklistScope == BL_AGENT);
    CHECK(c.blacklistTime == 30.0);
    CHECK(c.hasBlacklist());
    CHECK(c.visitChannels.ints == std::vector<int>{ 2 });
    CHECK(c.includeUnmatchedVisits);
    CHECK(c.bidControlType == BC_RELAY_FIXED);
    CHECK(c.fixedBidCpmInMicros == 2500);
    CHECK(c.providerConfig["demo"]["seat"] == 1);
    CHECK(c.winFormat == BRF_LIGHTWEIGHT);
    CHECK(c.lossFormat == BRF_NONE);
    CHECK(c.errorFormat == BRF_FULL);
    CHECK(c.ext["k"] == 1);
    CHECK(c.extensions["somethingNew"] == true);
}

TEST_CASE("agent config: hour of week bitmap")
{
    std::string bitmap(168, '0');
    bitmap[3 * 24 + 10] = '1';    // Wednesday 10h UTC

    AgentConfig c = parseWith(R"("hourOfWeekFilter": {"hourlyBitmapSundayMidnightUtc": ")"
                              + bitmap + R"("})");
    CHECK(!c.hourOfWeekFilter.isDefault());
    CHECK(c.hourOfWeekFilter.hourBitmap.count() == 1);

    using namespace std::chrono;
    CHECK(c.hourOfWeekFilter.isIncluded(sys_days{ year(2024) / 1 / 3 } + hours(10)));
    CHECK(!c.hourOfWeekFilter.isIncluded(sys_days{ year(2024) / 1 / 3 } + hours(11)));

    CHECK_THROWS_AS(parseWith(R"("hourOfWeekFilter": {"hourlyBitmapSundayMidnightUtc": "101"})"),
                    AgentConfigError);
    CHECK_THROWS_AS(parseWith(R"("hourOfWeekFilter": {"hourlyBitmapSundayMidnightUtc": ")"
                              + std::string(168, 'x') + R"("})"),
                    AgentConfigError);
}

TEST_CASE("agent config: creatives")
{
    AgentConfig c = parse(R"({
        "account": ["a"],
        "creatives": [
            { "id": 0, "format": "728x90", "name": "lb", "dealId": "DEAL1",
              "languageFilter": { "include": ["en"], "exclude": ["fr"] },
              "locationFilter": { "include": ["^US"] },
              "exchangeFilter": { "include": ["demo"] },
              "providerConfig": { "demo": { "adm": "x" } },
              "fees": { "type": "none" },
              "segmentFilter": { "interest": { "include": ["sports"],
                                               "excludeIfNotPresent": true } } },
            { "id": 1, "type": "video", "width": 640, "height": 480,
              "duration": 30, "bitrate": 1500 }
        ]
    })");

    const Creative& image = c.creatives[0];
    CHECK(image.isImage());
    CHECK(image.format == Format(728, 90));
    CHECK(image.dealId == "DEAL1");
    CHECK(image.languageFilter.include == std::vector<std::string>{ "en" });
    CHECK(image.languageFilter.exclude == std::vector<std::string>{ "fr" });
    CHECK(image.locationFilter.include[0].str == "^US");
    CHECK(image.exchangeFilter.include == std::vector<std::string>{ "demo" });
    CHECK(image.providerConfig["demo"]["adm"] == "x");
    CHECK(image.fees["type"] == "none");
    CHECK(image.segments.at("interest").excludeIfNotPresent);

    const Creative& video = c.creatives[1];
    CHECK(video.isVideo());
    CHECK(video.duration == 30);
    CHECK(video.bitrate == 1500);

    // Format compatibility.
    AdSpot spot;
    CHECK(image.compatible(spot));                   // no formats: anything goes
    spot.formats.push_back(Format(300, 250));
    CHECK(!image.compatible(spot));
    CHECK(Creative().compatible(spot));              // 0x0 matches anything
    spot.formats.push_back(Format(728, 90));
    CHECK(image.compatible(spot));
}

TEST_CASE("agent config: creative helpers")
{
    Creative img = Creative::image(300, 250, "bb", 4, "d");
    CHECK(img.isImage());
    CHECK(img.id == 4);
    CHECK(img.dealId == "d");

    Creative vid = Creative::video(640, 480, 15, 800, "v", 5);
    CHECK(vid.isVideo());
    CHECK(vid.duration == 15);
    CHECK(vid.bitrate == 800);
}

TEST_CASE("agent config: invalid configs")
{
    SECTION("structure")
    {
        CHECK_THROWS_AS(parse("not json"), AgentConfigError);
        CHECK_THROWS_AS(parse("[]"), AgentConfigError);
        CHECK_THROWS_AS(parse(R"({"creatives": [{"id": 0}]})"), AgentConfigError);   // no account
        CHECK_THROWS_AS(parse(R"({"account": ["a"]})"), AgentConfigError);           // no creatives
        CHECK_THROWS_AS(parse(R"({"account": ["a"], "creatives": []})"), AgentConfigError);
        CHECK_THROWS_AS(parseWith(R"("account": "bad slug!")"), AgentConfigError);
        CHECK_THROWS_AS(parseWith(R"("account": 5)"), AgentConfigError);
    }
    SECTION("values")
    {
        CHECK_THROWS_AS(parseWith(R"("bidProbability": 1.5)"), AgentConfigError);
        CHECK_THROWS_AS(parseWith(R"("bidProbability": -0.1)"), AgentConfigError);
        CHECK_THROWS_AS(parseWith(R"("minTimeAvailableMs": -1)"), AgentConfigError);
        CHECK_THROWS_AS(parseWith(R"("maxInFlight": -1)"), AgentConfigError);
        CHECK_THROWS_AS(parseWith(R"("maxInFlight": "many")"), AgentConfigError);
        CHECK_THROWS_AS(parseWith(R"("requiredIds": "xchg")"), AgentConfigError);
        CHECK_THROWS_AS(parseWith(R"("winFormat": "huge")"), AgentConfigError);
        CHECK_THROWS_AS(parseWith(R"("blacklist": {"type": "everyone"})"), AgentConfigError);
        CHECK_THROWS_AS(parseWith(R"("bidControl": {"type": "magic"})"), AgentConfigError);
    }
    SECTION("filters")
    {
        CHECK_THROWS_AS(parseWith(R"("urlFilter": {"allow": ["x"]})"), AgentConfigError);
        CHECK_THROWS_AS(parseWith(R"("urlFilter": {"include": "x"})"), AgentConfigError);
        CHECK_THROWS_AS(parseWith(R"("urlFilter": {"include": ["("]})"), AgentConfigError);
        CHECK_THROWS_AS(parseWith(R"("languageFilter": {"include": [1]})"), AgentConfigError);
        CHECK_THROWS_AS(parseWith(R"("foldPositionFilter": {"include": ["middle"]})"), AgentConfigError);
        CHECK_THROWS_AS(parseWith(R"("latLongDevFilter": [{"lat": 1}])"), AgentConfigError);
        CHECK_THROWS_AS(parseWith(R"("segmentFilter": {"s": {"bogus": 1}})"), AgentConfigError);
        CHECK_THROWS_AS(parseWith(R"("userPartition": {"hashOn": "moon"})"), AgentConfigError);
        CHECK_THROWS_AS(parseWith(R"("userPartition": {"modulus": 0})"), AgentConfigError);
        CHECK_THROWS_AS(parseWith(R"("userPartition": {"includeRanges": [[1]]})"), AgentConfigError);
        CHECK_THROWS_AS(parseWith(R"("augmentations": {"a": {"nope": 1}})"), AgentConfigError);
    }
    SECTION("creatives")
    {
        auto withCreative = [](const std::string& creative) {
            return parse(R"({"account": ["a"], "creatives": [)" + creative + "]}");
        };
        CHECK_THROWS_AS(withCreative(R"({"width": 1, "height": 1})"), AgentConfigError);  // no id
        CHECK_THROWS_AS(withCreative(R"({"id": 0, "type": "audio"})"), AgentConfigError);
        CHECK_THROWS_AS(withCreative(R"({"id": 0, "duration": 5})"), AgentConfigError);   // image
        CHECK_THROWS_AS(withCreative(R"({"id": 0, "format": "1x1", "width": 1})"), AgentConfigError);
        CHECK_THROWS_AS(withCreative(R"({"id": 0, "format": "big"})"), AgentConfigError);
        CHECK_THROWS_AS(withCreative(R"({"id": 0, "languageFilter": {"x": []}})"), AgentConfigError);
        CHECK_THROWS_AS(withCreative(R"({"id": 0, "segmentFilter": {"s": {"applyToExchanges": {}}}})"),
                        AgentConfigError);
        CHECK_THROWS_AS(withCreative("5"), AgentConfigError);
    }
}

TEST_CASE("agent config: sample config files are valid")
{
    for (const char* file : { "agent_minimal.json", "agent_full.json" }) {
        std::ifstream in(std::string(RTB_TEST_DATA_DIR) + "/" + file);
        REQUIRE(in.good());
        std::stringstream ss;
        ss << in.rdbuf();
        CHECK_NOTHROW(AgentConfig::parse(ss.str()));
    }
}

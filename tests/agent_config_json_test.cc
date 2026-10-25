// AgentConfig::toJson: round trips with the original JSON format.

#include "agent/agent_config.h"

#include "fixtures.h"

#include <catch2/catch_test_macros.hpp>

using namespace RTBKIT;
using namespace RTBKIT::Test;
using nlohmann::json;

namespace {

void checkRoundTrip(const AgentConfig& config)
{
    json j = config.toJson();
    AgentConfig back = AgentConfig::fromJson(json::parse(j.dump()));
    CHECK(back.toJson() == j);
    CHECK(back.name == config.name);
    CHECK(back.account == config.account);
    CHECK(back.creatives.size() == config.creatives.size());
}

} // namespace

TEST_CASE("agent config json: minimal")
{
    AgentConfig config = sampleAgentConfig("bob", "a:b");
    checkRoundTrip(config);

    AgentConfig back = AgentConfig::fromJson(config.toJson());
    CHECK(back.name == "bob");
    CHECK(back.account == AccountKey("a:b"));
    CHECK(back.creatives[0].format == Format(300, 250));
    CHECK(back.creatives[0].id == 0);
}

TEST_CASE("agent config json: data files")
{
    for (const char* file : {"agent_minimal.json", "agent_full.json"}) {
        DYNAMIC_SECTION(file) {
            json input = json::parse(readTestFile(file));
            // The data files may wrap the config in a "config" member.
            if (input.contains("config") && !input.contains("account")) input = input["config"];
            AgentConfig config = AgentConfig::fromJson(input);
            checkRoundTrip(config);
        }
    }
}

TEST_CASE("agent config json: every member survives")
{
    AgentConfig config = AgentConfig::parse(R"({
        "name": "full",
        "account": ["camp", "strat"],
        "externalId": 77,
        "external": true,
        "test": true,
        "requiredIds": ["xchg"],
        "roundRobin": {"group": "g", "weight": 3},
        "bidProbability": 0.25,
        "minTimeAvailableMs": 12.5,
        "maxInFlight": 7,
        "bidderInterface": "zmq",
        "hostFilter": {"include": ["example.com"], "exclude": ["bad.com"]},
        "urlFilter": {"include": ["^http://"]},
        "languageFilter": {"include": ["en"]},
        "locationFilter": {"exclude": ["^US"]},
        "exchangeFilter": {"include": ["openrtb"]},
        "foldPositionFilter": {"include": ["ABOVE", 3]},
        "latLongDevFilter": [{"lat": 1.5, "long": 2.5, "radius": 10}],
        "segmentFilter": {"audience": {"include": [1, "x"], "exclude": [2],
                                       "excludeIfNotPresent": true,
                                       "applyToExchanges": {"include": ["openrtb"]}}},
        "tagFilter": {"include": ["t"]},
        "hourOfWeekFilter": {"hourlyBitmapSundayMidnightUtc":
            "101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010"},
        "userPartition": {"hashOn": "ipua", "modulus": 10, "includeRanges": [[0, 3], [5, 7]]},
        "creatives": [
            {"id": 0, "format": "300x250", "name": "banner", "dealId": "d1",
             "providerConfig": {"openrtb": {"adm": "x"}},
             "languageFilter": {"include": ["en"]},
             "segmentFilter": {"audience": {"include": [1]}},
             "fees": {"a": 1}, "custom": 5},
            {"id": 1, "type": "video", "width": 640, "height": 480, "duration": 15, "bitrate": 500}
        ],
        "blacklist": {"type": "user_site", "time": 30, "scope": "agent"},
        "bidControl": {"type": "relay_fixed", "fixedBidCpmInMicros": 1500},
        "augmentations": {"frequency": {"config": {"max": 3}, "required": true,
                                        "filters": {"include": ["openrtb"]}}},
        "visits": {"channels": ["c1", 4], "includeUnmatched": true},
        "providerConfig": {"p": 1},
        "winFormat": "none", "lossFormat": "full", "errorFormat": "lightweight",
        "ext": {"e": 1},
        "unknownKey": [1, 2]
    })");

    json j = config.toJson();
    checkRoundTrip(config);

    AgentConfig back = AgentConfig::fromJson(j);
    CHECK(back.externalId == 77);
    CHECK(back.external);
    CHECK(back.test);
    CHECK(back.roundRobinGroup == "g");
    CHECK(back.bidProbability == 0.25f);
    CHECK(back.hostFilter.exclude.size() == 1);
    CHECK(back.foldPositionFilter.include == std::vector<int>{1, 3});
    CHECK(back.latLongDevFilter.latlonrads.size() == 1);
    CHECK(back.segments.at("audience").excludeIfNotPresent);
    CHECK(back.userPartition.hashOn == UserPartition::IPUA);
    CHECK(back.userPartition.includeRanges.size() == 2);
    CHECK(back.creatives[0].dealId == "d1");
    CHECK(back.creatives[0].extensions.at("custom") == 5);
    CHECK(back.creatives[1].isVideo());
    CHECK(back.creatives[1].duration == 15);
    CHECK(back.blacklistType == BL_USER_SITE);
    CHECK(back.blacklistScope == BL_AGENT);
    CHECK(back.bidControlType == BC_RELAY_FIXED);
    CHECK(back.fixedBidCpmInMicros == 1500);
    REQUIRE(back.augmentations.size() == 1);
    CHECK(back.augmentations[0].required);
    CHECK(back.includeUnmatchedVisits);
    CHECK(back.winFormat == BRF_NONE);
    CHECK(back.lossFormat == BRF_FULL);
    CHECK(back.extensions.at("unknownKey") == json::array({1, 2}));
    CHECK_FALSE(back.hourOfWeekFilter.isDefault());
    CHECK(back.hourOfWeekFilter.hourBitmap[0]);
    CHECK_FALSE(back.hourOfWeekFilter.hourBitmap[1]);
}

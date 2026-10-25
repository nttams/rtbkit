// Shared test fixtures for every area (include as "fixtures.h").
#pragma once

#include "agent/agent_config.h"
#include "common/auction.h"
#include "common/bid_request.h"
#include "common/events.h"
#include "common/openrtb_parser.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace RTBKIT::Test {

/** Contents of tests/data/<name>. */
inline std::string readTestFile(const std::string& name)
{
    std::ifstream in(std::string(RTB_TEST_DATA_DIR) + "/" + name);
    REQUIRE(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/** A request with two spots (ids "1" and "2"), exchange "test-exchange", 100 ms
    to answer, a 0.5 CPM floor on spot 0 and no floor on spot 1. */
inline std::shared_ptr<BidRequest> sampleBidRequest(const std::string& auctionId = "auction-1")
{
    auto request = std::make_shared<BidRequest>();
    request->auctionId = Id(auctionId);
    request->exchange = "test-exchange";
    request->provider = "test-exchange";
    request->timeAvailableMs = 100;
    request->url = Url("http://www.example.com/page");
    request->bidCurrency.push_back(CurrencyCode::USD);
    for (int i = 0; i < 2; ++i) {
        AdSpot spot;
        spot.id = Id(uint64_t(i + 1));
        spot.formats.push_back(Format(300, 250));
        spot.isBanner = true;
        if (i == 0) spot.reservePrice = USD_CPM(0.5);
        request->imp.push_back(spot);
    }
    return request;
}

/** A valid agent config with one 300x250 creative (id 0). */
inline AgentConfig sampleAgentConfig(const std::string& name = "agent1",
                                     const std::string& account = "campaign:strategy")
{
    AgentConfig config = AgentConfig::parse(
        R"({"account": ")" + account + R"(", "creatives": [{"id": 0, "format": "300x250"}]})");
    config.name = name;
    return config;
}

/** A response from sampleAgentConfig() for the given spot. */
inline Response sampleResponse(int spot, double cpm, const std::string& agent = "agent1",
                               int priority = 0)
{
    Response r;
    r.agent = agent;
    r.account = AccountKey("campaign:strategy");
    r.price = USD_CPM(cpm);
    r.spotIndex = spot;
    r.creativeIndex = 0;
    r.priority = priority;
    r.bidData = "data-" + agent;
    return r;
}

} // namespace RTBKIT::Test

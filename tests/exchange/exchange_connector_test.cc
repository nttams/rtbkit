// ExchangeConnector interface: providerData and the auction handler.

#include "exchange/exchange_connector.h"

#include "fixtures.h"

#include <catch2/catch_test_macros.hpp>

using namespace RTBKIT;
using namespace RTBKIT::Test;

namespace {

struct TestConnector : public ExchangeConnector {
    TestConnector() : ExchangeConnector("test-exchange") {}

    void configure(const nlohmann::json&) override {}
    void start() override {}
    void stop() override {}

    void receive(std::shared_ptr<Auction> auction) { submit(std::move(auction)); }

    bool rejectConfigs = false;
    int rejectCreative = -1;

    std::shared_ptr<void> getProviderData(const AgentConfig&) const override
    {
        return rejectConfigs ? nullptr : std::make_shared<int>(42);
    }

    std::shared_ptr<void> getCreativeProviderData(
            const AgentConfig&, const Creative& creative) const override
    {
        return creative.id == rejectCreative ? nullptr : std::make_shared<int>(creative.id);
    }
};

} // namespace

TEST_CASE("exchange connector: prepareConfig fills providerData")
{
    TestConnector connector;
    AgentConfig config = AgentConfig::parse(
        R"({"account": "a", "creatives": [{"id": 0, "format": "300x250"},
                                           {"id": 1, "format": "728x90"}]})");

    REQUIRE(connector.prepareConfig(config));
    REQUIRE(config.providerData.count("test-exchange"));
    CHECK(*std::static_pointer_cast<int>(config.providerData["test-exchange"]) == 42);
    CHECK(config.creatives[1].providerData.count("test-exchange") == 1);

    // A creative the exchange can not use gets no entry.
    connector.rejectCreative = 1;
    REQUIRE(connector.prepareConfig(config));
    CHECK(config.creatives[0].providerData.count("test-exchange") == 1);
    CHECK(config.creatives[1].providerData.count("test-exchange") == 0);

    // A config the exchange can not use gets nothing.
    AgentConfig other = sampleAgentConfig();
    connector.rejectConfigs = true;
    CHECK_FALSE(connector.prepareConfig(other));
    CHECK(other.providerData.empty());
}

TEST_CASE("exchange connector: default provider data is a non-null marker")
{
    struct Plain : public ExchangeConnector {
        Plain() : ExchangeConnector("plain") {}
        void configure(const nlohmann::json&) override {}
        void start() override {}
        void stop() override {}
    } connector;

    AgentConfig config = sampleAgentConfig();
    REQUIRE(connector.prepareConfig(config));
    CHECK(config.providerData.at("plain") != nullptr);
    CHECK(connector.exchangeName() == "plain");
    CHECK(connector.port() == 0);
}

TEST_CASE("exchange connector: submit goes to the auction handler")
{
    TestConnector connector;
    auto auction = Auction::create(sampleBidRequest());

    CHECK_THROWS_AS(connector.receive(auction), std::logic_error);

    std::shared_ptr<Auction> received;
    connector.setAuctionHandler([&](std::shared_ptr<Auction> a) { received = std::move(a); });
    connector.receive(auction);
    CHECK(received == auction);
}

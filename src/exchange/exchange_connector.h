// Interface between the router and an exchange connector (OpenRTB over HTTP,
// the mock exchange...). Simplified from rtbkit/common/exchange_connector.h.
//
//   exchange --HTTP bid request--> connector
//     connector parses it into a BidRequest and creates the Auction with a
//     completion callback, then hands it to the router:  submit(auction)
//   router fans out, collects bids, picks winners, calls auction->finish()
//   completion callback (on the router thread) --> connector builds the HTTP
//     bid response from auction->bestResponse(spot) and wakes the HTTP thread.
//
// Threading: the connector owns its HTTP threads. submit() may be called from
// any of them; the router takes the auction over on its own thread. The
// completion callback runs on the router thread, so it must only hand the
// result to the waiting HTTP thread (promise / condition variable) and return.
// The router always finishes an auction by its expiry, even with no bids, so a
// waiting HTTP thread is never stuck.
#pragma once

#include "common/auction.h"
#include "common/exchange_filter_hooks.h"

#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

namespace RTBKIT {

struct AgentConfig;
struct Creative;

/** The router's entry point for new auctions. */
using AuctionHandler = std::function<void(std::shared_ptr<Auction>)>;

class ExchangeConnector : public ExchangeFilterHooks {
public:
    explicit ExchangeConnector(std::string name) : name_(std::move(name)) {}

    /** The name used by exchangeFilter in agent configs and in
        BidRequest::exchange, e.g. "openrtb". */
    std::string exchangeName() const override { return name_; }

    /** Reads the connector settings (listening port, win notice url...).
        Called once, before start(). Throws std::runtime_error when invalid. */
    virtual void configure(const nlohmann::json& config) = 0;

    /** Starts the HTTP server thread(s). Returns when the connector is ready
        to accept requests. */
    virtual void start() = 0;

    /** Stops and joins all threads. Safe to call twice. */
    virtual void stop() = 0;

    /** The port the connector listens on once started (0 before). */
    virtual int port() const { return 0; }

    /** Set by the router (or the demo) before start(). */
    void setAuctionHandler(AuctionHandler handler) { handler_ = std::move(handler); }

    /** Provider data for an agent config: whatever the connector needs later
        to answer for this config (e.g. parsed providerConfig). Return null
        if the config can not be used with this exchange; it then gets no
        entry and every exchange filter rejects it. The default is a non-null
        marker, i.e. "compatible, nothing to remember". */
    virtual std::shared_ptr<void> getProviderData(const AgentConfig& config) const;

    /** Same for one creative of the config. Null means the creative can not
        be used on this exchange. */
    virtual std::shared_ptr<void> getCreativeProviderData(
            const AgentConfig& config, const Creative& creative) const;

    /** Fills config.providerData[exchangeName()] and the same entry of every
        creative using the two hooks above. The router calls it for each
        CONFIG it receives, before the config goes to the FilterPool: the
        exchange filters reject configs without providerData.
        Returns false (nothing filled) if the config is not compatible. */
    bool prepareConfig(AgentConfig& config) const;

protected:
    /** Hands an auction to the router. Throws std::logic_error if no handler
        was set. */
    void submit(std::shared_ptr<Auction> auction) const;

    /** For connectors whose name comes from configure(). Call before the
        connector is registered with the router. */
    void setExchangeName(std::string name) { name_ = std::move(name); }

private:
    std::string name_;
    AuctionHandler handler_;
};

} // namespace RTBKIT

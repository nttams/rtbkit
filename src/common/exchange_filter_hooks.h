// The part of an exchange connector the filters see. The full connector
// interface (exchange/exchange_connector.h) derives from it.
#pragma once

#include <string>

namespace RTBKIT {

struct BidRequest;
struct AgentConfig;
struct Creative;

struct ExchangeFilterHooks {
    virtual ~ExchangeFilterHooks() = default;

    virtual std::string exchangeName() const = 0;

    /** Returns true if the config should be kept. */
    virtual bool bidRequestPreFilter(
            const BidRequest&, const AgentConfig&, const void* /*providerData*/) const
    {
        return true;
    }

    virtual bool bidRequestPostFilter(
            const BidRequest&, const AgentConfig&, const void* /*providerData*/) const
    {
        return true;
    }

    /** Returns true if the creatives described by the provider data may be
        used for the request. */
    virtual bool bidRequestCreativeFilter(
            const BidRequest&, const AgentConfig&, const void* /*providerData*/) const
    {
        return true;
    }
};

} // namespace RTBKIT

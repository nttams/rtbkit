// Minimal stand-in for rtbkit/common/exchange_connector.h. The ExchangePre and
// ExchangePost filters only need these three calls.
#pragma once

#include <string>

namespace RTBKIT {

struct BidRequest;
struct AgentConfig;

struct ExchangeConnector {
    virtual ~ExchangeConnector() {}

    virtual std::string exchangeName() const = 0;

    /** Returns true if the config should be kept. */
    virtual bool bidRequestPreFilter(
            const BidRequest&, const AgentConfig&, const void*) const
    {
        return true;
    }

    virtual bool bidRequestPostFilter(
            const BidRequest&, const AgentConfig&, const void*) const
    {
        return true;
    }
};

} // namespace RTBKIT

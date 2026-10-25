#include "exchange/exchange_connector.h"

#include "agent/agent_config.h"

#include <stdexcept>

namespace RTBKIT {

std::shared_ptr<void> ExchangeConnector::getProviderData(const AgentConfig&) const
{
    return std::make_shared<bool>(true);
}

std::shared_ptr<void> ExchangeConnector::getCreativeProviderData(
        const AgentConfig&, const Creative&) const
{
    return std::make_shared<bool>(true);
}

bool ExchangeConnector::prepareConfig(AgentConfig& config) const
{
    auto data = getProviderData(config);
    if (!data) return false;

    config.providerData[exchangeName()] = std::move(data);
    for (Creative& creative : config.creatives) {
        auto creativeData = getCreativeProviderData(config, creative);
        if (creativeData)
            creative.providerData[exchangeName()] = std::move(creativeData);
        else
            creative.providerData.erase(exchangeName());
    }
    return true;
}

void ExchangeConnector::submit(std::shared_ptr<Auction> auction) const
{
    if (!handler_)
        throw std::logic_error("exchange connector " + name_ + " has no auction handler");
    handler_(std::move(auction));
}

} // namespace RTBKIT

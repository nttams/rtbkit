// Port of rtbkit/core/router/filter_pool.h: just the filter-running core.
// No events, no monitoring, no threading, no lock-free data swapping.
#pragma once

#include "agent_config.h"
#include "filter.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace RTBKIT {

struct FilterPool
{
    struct ConfigEntry
    {
        void reset()
        {
            name = "";
            config.reset();
        }

        std::string name;
        std::shared_ptr<AgentConfig> config;
    };

    struct Result
    {
        // Names of the configs that passed every filter.
        std::vector<std::string> matched;

        // (config name, name of the filter that dropped it).
        std::vector< std::pair<std::string, std::string> > rejected;
    };

    Result filter(
            const BidRequest& br,
            const ExchangeConnector* conn,
            const ConfigSet& mask = ConfigSet(true)) const;

    // Filters are kept sorted by priority.
    void addFilter(std::unique_ptr<FilterBase> filter);
    void addFilter(const std::string& name);
    void removeFilter(const std::string& name);
    void initWithDefaultFilters();

    unsigned addConfig(const std::string& name,
                       const std::shared_ptr<AgentConfig>& config);
    void removeConfig(const std::string& name);

    std::vector<std::string> getFilterNames() const;

private:
    std::ptrdiff_t findConfig(const std::string& name) const;
    std::ptrdiff_t findFilter(const std::string& name) const;

    std::vector< std::unique_ptr<FilterBase> > filters;
    std::vector<ConfigEntry> configs;
    ConfigSet activeConfigs;
};

} // namespace RTBKIT

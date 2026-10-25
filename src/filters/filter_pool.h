// Port of rtbkit/core/router/filter_pool.h: holds the set of filters and the
// registered agent configs and runs a bid request through all of them. No
// events, no monitoring, no threading, no lock-free data swapping: callers
// serialize access themselves.
#pragma once

#include "agent/agent_config.h"
#include "filters/filter.h"

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

    /** A config that passed every filter and what it may bid on. */
    struct Match
    {
        std::string config;

        /** Eligible creatives for each ad spot of the request, ordered by
            spot index. Spots without any eligible creative are absent. */
        BiddableSpots spots;
    };

    struct Result
    {
        /// Configs that passed every filter, in registration slot order.
        std::vector<Match> matched;

        /// (config name, name of the filter that dropped it).
        std::vector< std::pair<std::string, std::string> > rejected;

        std::vector<std::string> matchedNames() const;
    };

    /** Runs the filters. Only configs in the mask are considered; configs
        outside of it are neither matched nor reported as rejected. Throws
        std::runtime_error when no filter is registered. */
    Result filter(
            const BidRequest& br,
            const ExchangeFilterHooks* conn,
            const ConfigSet& mask = ConfigSet(true)) const;

    // Filters are kept sorted by priority.
    void addFilter(std::unique_ptr<FilterBase> filter);

    /** Adds a static or creative filter by name. Throws on unknown names. */
    void addFilter(const std::string& name);
    void removeFilter(const std::string& name);

    /** Replaces the filters by every static and creative filter. */
    void initWithDefaultFilters();

    /** Registers or replaces the config of that name. Returns its slot. */
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

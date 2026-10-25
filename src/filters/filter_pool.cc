#include "filters/filter_pool.h"
#include "filters/creative_filters.h"
#include "filters/static_filters.h"

#include <algorithm>

using namespace std;

namespace RTBKIT {

vector<string>
FilterPool::Result::
matchedNames() const
{
    vector<string> names;
    for (const auto& match : matched) names.push_back(match.config);
    return names;
}

FilterPool::Result
FilterPool::
filter(const BidRequest& br, const ExchangeConnector* conn, const ConfigSet& mask) const
{
    RTB_CHECK(!filters.empty(), "No filters registered");

    // Every creative of every active config starts out eligible.
    CreativeMatrix active;
    for (size_t i = activeConfigs.next(); i < activeConfigs.size();
         i = activeConfigs.next(i + 1))
        active.setConfig(i, configs[i].config->creatives.size());

    FilterState state(br, conn, active);
    state.narrowConfigs(mask);

    Result result;
    ConfigSet before = state.configs();

    for (const auto& filter : filters) {
        filter->filter(state);

        const ConfigSet& after = state.configs();

        // Everything that was in before and is no longer in after was
        // dropped by this filter.
        ConfigSet dropped = before ^ (before & after);
        for (size_t i = dropped.next(); i < dropped.size(); i = dropped.next(i + 1))
            result.rejected.emplace_back(configs[i].name, filter->name());
        before = after;

        state.resetFilterReasons();

        if (after.empty()) break;
    }

    // Creatives of dropped configs are cleaned up here. A config with no
    // spot to bid on (a request without impressions) can't match.
    auto biddable = state.biddableSpots();

    const ConfigSet& left = state.configs();
    for (size_t i = left.next(); i < left.size(); i = left.next(i + 1)) {
        auto it = biddable.find(unsigned(i));
        if (it == biddable.end() || it->second.empty()) {
            result.rejected.emplace_back(configs[i].name, "NoBiddableSpots");
            continue;
        }
        result.matched.push_back({ configs[i].name, std::move(it->second) });
    }

    return result;
}


void
FilterPool::
addFilter(unique_ptr<FilterBase> filter)
{
    ConfigSet active = activeConfigs;
    for (size_t cfgId = active.next();
         cfgId < active.size();
         cfgId = active.next(cfgId + 1))
    {
        filter->addConfig(unsigned(cfgId), configs[cfgId].config);
    }

    filters.push_back(std::move(filter));
    stable_sort(filters.begin(), filters.end(),
            [] (const unique_ptr<FilterBase>& lhs, const unique_ptr<FilterBase>& rhs) {
                return lhs->priority() < rhs->priority();
            });
}

void
FilterPool::
addFilter(const string& name)
{
    auto filter = createStaticFilter(name);
    if (!filter) filter = createCreativeFilter(name);
    RTB_CHECK(filter != nullptr, "unknown filter: " + name);
    addFilter(std::move(filter));
}

void
FilterPool::
removeFilter(const string& name)
{
    ptrdiff_t index = findFilter(name);
    if (index < 0) return;
    filters.erase(filters.begin() + index);
}

void
FilterPool::
initWithDefaultFilters()
{
    filters.clear();
    for (const auto& name : staticFilterNames()) addFilter(name);
    for (const auto& name : creativeFilterNames()) addFilter(name);
}

unsigned
FilterPool::
addConfig(const string& name, const shared_ptr<AgentConfig>& config)
{
    RTB_CHECK(config != nullptr, "null config: " + name);

    // If our config already exists, we have to deregister it with the filters
    // before we can add the new config.
    removeConfig(name);

    // Reuse the first free slot.
    ptrdiff_t index = findConfig("");
    if (index >= 0) {
        configs[index].name = name;
        configs[index].config = config;
    }
    else {
        index = configs.size();
        configs.push_back({ name, config });
    }

    activeConfigs.set(index);

    for (const auto& filter : filters)
        filter->addConfig(unsigned(index), config);

    return unsigned(index);
}

void
FilterPool::
removeConfig(const string& name)
{
    ptrdiff_t index = findConfig(name);
    if (index < 0) return;

    activeConfigs.reset(index);

    for (const auto& filter : filters)
        filter->removeConfig(unsigned(index), configs[index].config);

    configs[index].reset();
}

vector<string>
FilterPool::
getFilterNames() const
{
    vector<string> names;
    for (const auto& filter : filters) names.push_back(filter->name());
    return names;
}

ptrdiff_t
FilterPool::
findConfig(const string& name) const
{
    for (size_t i = 0; i < configs.size(); ++i) {
        if (configs[i].name == name) return i;
    }
    return -1;
}

ptrdiff_t
FilterPool::
findFilter(const string& name) const
{
    for (size_t i = 0; i < filters.size(); ++i) {
        if (filters[i]->name() == name) return i;
    }
    return -1;
}

} // namespace RTBKIT

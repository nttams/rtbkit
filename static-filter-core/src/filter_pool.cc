// Port of rtbkit/core/router/filter_pool.cc: just the filter-running core.
#include "filter_pool.h"
#include "static_filters.h"

#include <algorithm>

using namespace std;

namespace RTBKIT {

FilterPool::Result
FilterPool::
filter(const BidRequest& br, const ExchangeConnector* conn, const ConfigSet& mask) const
{
    ExcCheck(!filters.empty(), "No filters registered");

    FilterState state(br, conn, activeConfigs);
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

    const ConfigSet& left = state.configs();
    for (size_t i = left.next(); i < left.size(); i = left.next(i + 1))
        result.matched.push_back(configs[i].name);

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
        filter->addConfig(cfgId, configs[cfgId].config);
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
    ExcCheck(filter != nullptr, "unknown filter: " + name);
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
}

unsigned
FilterPool::
addConfig(const string& name, const shared_ptr<AgentConfig>& config)
{
    // If our config already exists, we have to deregister it with the filters
    // before we can add the new config.
    removeConfig(name);

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
        filter->addConfig(index, config);

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
        filter->removeConfig(index, configs[index].config);

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

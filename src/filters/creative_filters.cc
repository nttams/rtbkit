#include "filters/creative_filters.h"

using namespace std;

namespace RTBKIT {

/******************************************************************************/
/* CREATIVE EXCHANGE FILTER                                                   */
/******************************************************************************/

void
CreativeExchangeFilter::
filter(FilterState& state) const
{
    // No exchange connector means everything gets filtered out.
    if (!state.exchange) {
        state.narrowAllCreatives(CreativeMatrix());
        return;
    }

    CreativeMatrix creatives;

    for (size_t cfgId = state.configs().next();
         cfgId < state.configs().size();
         cfgId = state.configs().next(cfgId + 1))
    {
        const auto& config = *configs[cfgId];

        for (size_t crId = 0; crId < config.creatives.size(); ++crId) {
            const auto& creative = config.creatives[crId];

            auto it = creative.providerData.find(state.exchange->exchangeName());
            if (it == creative.providerData.end()) continue;

            if (state.exchange->bidRequestCreativeFilter(
                        state.request, config, it->second.get()))
                creatives.set(crId, cfgId);
        }
    }

    state.narrowAllCreatives(creatives);
}


/******************************************************************************/
/* CREATIVE SEGMENTS FILTER                                                   */
/******************************************************************************/

void
CreativeSegmentsFilter::
filter(FilterState& state) const
{
    unordered_set<string> toCheck = excludeIfNotPresent;

    for (const auto& segment : state.request.segments) {
        toCheck.erase(segment.first);

        auto it = data.find(segment.first);
        if (it == data.end()) continue;

        CreativeMatrix result = it->second.ie.filter(*segment.second);
        state.narrowAllCreatives(result);

        if (state.configs().empty()) return;
    }

    for (const auto& segment : toCheck) {
        auto it = data.find(segment);
        if (it == data.end()) continue;

        CreativeMatrix result = it->second.excludeIfNotPresent.negate();
        state.narrowAllCreatives(result);
        if (state.configs().empty()) return;
    }
}

void
CreativeSegmentsFilter::
setCreative(unsigned configIndex, unsigned crIndex, const Creative& creative, bool value)
{
    for (const auto& entry : creative.segments) {
        auto& segment = data[entry.first];

        segment.ie.setInclude(configIndex, crIndex, value, entry.second.include);
        segment.ie.setExclude(configIndex, crIndex, value, entry.second.exclude);

        if (entry.second.excludeIfNotPresent) {
            if (value && segment.excludeIfNotPresent.empty())
                excludeIfNotPresent.insert(entry.first);

            segment.excludeIfNotPresent.set(crIndex, configIndex, value);

            if (!value && segment.excludeIfNotPresent.empty())
                excludeIfNotPresent.erase(entry.first);
        }
    }
}


/******************************************************************************/
/* FACTORY                                                                    */
/******************************************************************************/

template<typename F>
static std::unique_ptr<FilterBase> make() { return std::make_unique<F>(); }

std::unique_ptr<FilterBase>
createCreativeFilter(const std::string& name)
{
    static const std::unordered_map<std::string, std::unique_ptr<FilterBase> (*)()>
    factories = {
        { CreativeFormatFilter::name,       make<CreativeFormatFilter> },
        { CreativeLanguageFilter::name,     make<CreativeLanguageFilter> },
        { CreativeLocationFilter::name,     make<CreativeLocationFilter> },
        { CreativeExchangeNameFilter::name, make<CreativeExchangeNameFilter> },
        { CreativeExchangeFilter::name,     make<CreativeExchangeFilter> },
        { CreativeSegmentsFilter::name,     make<CreativeSegmentsFilter> },
        { CreativePMPFilter::name,          make<CreativePMPFilter> },
    };

    auto it = factories.find(name);
    return it == factories.end() ? nullptr : it->second();
}

const std::vector<std::string>&
creativeFilterNames()
{
    static const std::vector<std::string> names = {
        CreativeFormatFilter::name, CreativeLanguageFilter::name,
        CreativeLocationFilter::name, CreativeExchangeNameFilter::name,
        CreativeExchangeFilter::name, CreativeSegmentsFilter::name,
        CreativePMPFilter::name };
    return names;
}

} // namespace RTBKIT

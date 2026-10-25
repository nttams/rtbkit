// Port of rtbkit/core/router/filters/static_filters.cc
#include "filters/static_filters.h"
#include "filters/md5.h"

#include <cstring>
#include <limits>
#include <random>

using namespace std;

namespace RTBKIT {

/******************************************************************************/
/* SEGMENT FILTER                                                             */
/******************************************************************************/

void
SegmentsFilter::
setConfig(unsigned cfgIndex, const AgentConfig& config, bool value)
{
    for (const auto& entry : config.segments) {
        auto& segment = data[entry.first];

        segment.ie.setInclude(cfgIndex, value, entry.second.include);
        segment.ie.setExclude(cfgIndex, value, entry.second.exclude);

        segment.exchange.setIncludeExclude(
                cfgIndex, value, entry.second.applyToExchanges);

        if (entry.second.excludeIfNotPresent) {
            if (value && segment.excludeIfNotPresent.empty())
                excludeIfNotPresent.insert(entry.first);

            segment.excludeIfNotPresent.set(cfgIndex, value);

            if (!value && segment.excludeIfNotPresent.empty())
                excludeIfNotPresent.erase(entry.first);
        }
    }
}

ConfigSet
SegmentsFilter::SegmentData::
applyExchangeFilter(FilterState& state, const ConfigSet& result) const
{
    ConfigSet current = state.configs();

    /* This is a bit tricky because our filter mechanism doesn't gracefully
       support skipping filters which is required for the exchange IE. So
       instead we'll take the result of the original filter and massage it until
       we get the mask we want.
       First off, let's figure out which configs would change from 1 to 0 if we
       applied result to the state.
    */
    ConfigSet affected = current ^ (current & result);
    if (affected.empty()) return ConfigSet(true);

    /* Out of those configs, let's remove the ones that we should skip. Note
       that the filter will return all the configs that should not be
       skipped.
    */
    ConfigSet leftover = affected & exchange.filter(state.request.exchange);

    /* At this point we have a 1 in our leftover bitfield for each configs
       that would be filtered out and is not marked for skipping. We can
       therefor remove those configs from state by negating the bitfield.
    */
    return leftover.negate();
}


void
SegmentsFilter::
fillFilterReasons(FilterState& state, ConfigSet& beforeFilt,
                  ConfigSet& afterFilt, const std::string & segment) const {

    // Get all the filtered out configs by this segment.
    FilterState::FilterReasons& reasons = state.getFilterReasons();
    reasons[segment] = beforeFilt ^ (beforeFilt & afterFilt);
}

void
SegmentsFilter::
filter(FilterState& state) const
{
    unordered_set<string> toCheck = excludeIfNotPresent;

    for (const auto& segment : state.request.segments) {
        toCheck.erase(segment.first);

        auto it = data.find(segment.first);
        if (it == data.end()) continue;

        ConfigSet beforeFilt = state.configs();
        ConfigSet result = it->second.ie.filter(*segment.second);

        ConfigSet result2 = it->second.applyExchangeFilter(state, result);
        state.narrowConfigs(result2);

        fillFilterReasons(state, beforeFilt, result2, segment.first);

        if (state.configs().empty()) return;
    }

    for (const auto& segment : toCheck) {
        auto it = data.find(segment);
        if (it == data.end()) continue;

        ConfigSet result = it->second.excludeIfNotPresent.negate();
        ConfigSet result2 = it->second.applyExchangeFilter(state, result);
        ConfigSet beforeFilt = state.configs();
        state.narrowConfigs(result2);
        fillFilterReasons(state, beforeFilt, result2, segment);
        if (state.configs().empty()) return;
    }
}


/******************************************************************************/
/* USER PARTITION FILTER                                                      */
/******************************************************************************/

void
UserPartitionFilter::
setConfig(unsigned cfgIndex, const AgentConfig& config, bool value)
{
    const auto& part = config.userPartition;

    if (part.hashOn == UserPartition::NONE) {
        defaultSet.set(cfgIndex, value);
        return;
    }

    int uid = int(config.externalId);
    auto& entry = data[getKey(part, uid)];
    entry.uid = uid;
    if (entry.hashOn == UserPartition::NONE) {
        entry.modulus = part.modulus;
        entry.hashOn = part.hashOn;
    }
    RTB_ASSERT(entry.modulus == part.modulus);
    RTB_ASSERT(entry.hashOn == part.hashOn);

    entry.excludeIfEmpty.set(cfgIndex);

    if (value) entry.filter.addConfig(cfgIndex, part.includeRanges);
    else entry.filter.removeConfig(cfgIndex, part.includeRanges);
}

void
UserPartitionFilter::
filter(FilterState& state) const
{
    ConfigSet matches = defaultSet;
    ConfigSet excludes;

    for (const auto& entry : data) {
        auto value = getValue(state.request, entry.second);

        if (!value.first) excludes |= entry.second.excludeIfEmpty;
        else matches |= entry.second.filter.filter(int(value.second));
    }

    matches &= excludes.negate();
    state.narrowConfigs(matches);
}

namespace {

// First 8 bytes of the MD5 digest, read as a native-endian uint64.
uint64_t calcHash(const std::string& str)
{
    auto digest = md5(str);
    uint64_t result;
    memcpy(&result, digest.data(), sizeof(result));
    return result;
}

} // namespace anonymous

std::pair<bool, uint64_t>
UserPartitionFilter::
getValue(const BidRequest& br, const FilterEntry& entry) const
{
    if (entry.hashOn == UserPartition::RANDOM) {
        thread_local std::mt19937_64 rng{std::random_device{}()};
        return make_pair(true, rng() % entry.modulus);
    }

    string str;

    switch (entry.hashOn) {
    case UserPartition::EXCHANGEID:
        str = br.userIds.exchangeId.toString(); break;

    case UserPartition::PROVIDERID:
        str = br.userIds.providerId.toString(); break;

    case UserPartition::IPUA:
        str = br.ipAddress + br.userAgent; break;

    default: RTB_ASSERT(false);
    };

    if (str.empty() || str == "null") return make_pair(false, 0);

    auto h = calcHash(str) + entry.uid;
    return make_pair(true, h % entry.modulus);
}


/******************************************************************************/
/* EXCHANGE PRE/POST FILTER                                                   */
/******************************************************************************/

bool
ExchangePreFilter::
filterConfig(FilterState& state, const AgentConfig& config) const
{
    if (!state.exchange) return false;

    auto it = config.providerData.find(state.exchange->exchangeName());
    if (it == config.providerData.end()) return false;

    return state.exchange->bidRequestPreFilter(
            state.request, config, it->second.get());
}

bool
ExchangePostFilter::
filterConfig(FilterState& state, const AgentConfig& config) const
{
    if (!state.exchange) return false;

    auto it = config.providerData.find(state.exchange->exchangeName());
    if (it == config.providerData.end()) return false;

    return state.exchange->bidRequestPostFilter(
            state.request, config, it->second.get());
}


/******************************************************************************/
/* LAT LONG DEV FILTER                                                        */
/******************************************************************************/

LatLongDevFilter::Square
LatLongDevFilter::squareFromLatLongRadius(float lat, float lon, float radius)
{
    Square sq;
    sq.y_max = (lat * LATITUDE_1DEGREE_KMS) + radius;
    sq.y_min = (lat * LATITUDE_1DEGREE_KMS) - radius;

    sq.x_max = ( lon * LONGITUDE_1DEGREE_KMS * cosInDegrees(lat) ) + radius;
    sq.x_min = ( lon * LONGITUDE_1DEGREE_KMS * cosInDegrees(lat) ) - radius;

    return sq;
}

void LatLongDevFilter::addConfig(unsigned cfgIndex,
        const std::shared_ptr<AgentConfig>& config)
{
    SquareList squares;
    for (const LatLonRad & llr : config->latLongDevFilter.latlonrads) {
        auto sq = squareFromLatLongRadius(llr.lat, llr.lon, llr.radius);
        squares.push_back(sq);
    }
    if (!squares.empty()) {
        squares_by_confindx[cfgIndex] = squares;
        configs_with_filt.set(cfgIndex);
    }
}

void LatLongDevFilter::removeConfig(unsigned cfgIndex,
        const std::shared_ptr<AgentConfig>&)
{
    if (squares_by_confindx.count(cfgIndex) > 0) {
        squares_by_confindx.erase(cfgIndex);
        configs_with_filt.reset(cfgIndex);
    }
}

void LatLongDevFilter::filter(FilterState& state) const
{
    ConfigSet matches = state.configs();

    if (!checkLatLongPresent(state.request)) {
        // If there is no geo info the filter, then filter out all the
        // agent configs that has this filter present.
        state.narrowConfigs(configs_with_filt.negate());
    } else {
        // Filter using the lat long of the request and from the configs
        // of the agents.
        for (const auto& entry : squares_by_confindx) {
            if (pointInsideAnySquare(state.request.device->geo->lat,
                    state.request.device->geo->lon, entry.second)) {
                continue;
            }
            matches.reset(entry.first);
        }
        state.narrowConfigs(matches);
    }
}

bool LatLongDevFilter::checkLatLongPresent(const BidRequest & req) const
{
    if (!req.device) return false;
    if (!req.device->geo) return false;
    if (std::isnan(req.device->geo->lat) || std::isnan(req.device->geo->lon))
        return false;
    return true;
}

bool
LatLongDevFilter::pointInsideAnySquare(float lat, float lon,
        const SquareList & squares)
{
    const float y = lat * LATITUDE_1DEGREE_KMS;
    const float x = lon * LONGITUDE_1DEGREE_KMS * cosInDegrees(lat);
    for (const auto& sq : squares) {
        if (insideSquare(x, y , sq)) return true;
    }
    return false;
}


/******************************************************************************/
/* FACTORY                                                                    */
/******************************************************************************/

template<typename F>
static std::unique_ptr<FilterBase> make() { return std::make_unique<F>(); }

std::unique_ptr<FilterBase>
createStaticFilter(const std::string& name)
{
    static const std::unordered_map<std::string, std::unique_ptr<FilterBase> (*)()>
    factories = {
        { SegmentsFilter::name,      make<SegmentsFilter> },
        { UserPartitionFilter::name, make<UserPartitionFilter> },
        { HourOfWeekFilter::name,    make<HourOfWeekFilter> },
        { UrlFilter::name,           make<UrlFilter> },
        { HostFilter::name,          make<HostFilter> },
        { LanguageFilter::name,      make<LanguageFilter> },
        { LocationFilter::name,      make<LocationFilter> },
        { ExchangePreFilter::name,   make<ExchangePreFilter> },
        { ExchangePostFilter::name,  make<ExchangePostFilter> },
        { ExchangeNameFilter::name,  make<ExchangeNameFilter> },
        { FoldPositionFilter::name,  make<FoldPositionFilter> },
        { RequiredIdsFilter::name,   make<RequiredIdsFilter> },
        { LatLongDevFilter::name,    make<LatLongDevFilter> },
    };

    auto it = factories.find(name);
    return it == factories.end() ? nullptr : it->second();
}

const std::vector<std::string>&
staticFilterNames()
{
    static const std::vector<std::string> names = {
        SegmentsFilter::name, UserPartitionFilter::name, HourOfWeekFilter::name,
        UrlFilter::name, HostFilter::name, LanguageFilter::name,
        LocationFilter::name, ExchangePreFilter::name, ExchangePostFilter::name,
        ExchangeNameFilter::name, FoldPositionFilter::name,
        RequiredIdsFilter::name, LatLongDevFilter::name };
    return names;
}

} // namespace RTBKIT

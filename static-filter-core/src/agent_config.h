// Port of rtbkit/core/agent_configuration/agent_config.h (only the fields the
// static filters use).
#pragma once

#include "bid_request.h"
#include "include_exclude.h"
#include "segments.h"

#include <bitset>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace RTBKIT {

struct LatLonRad {
    LatLonRad() {}
    LatLonRad(float la, float lo, float r) : lat(la), lon(lo), radius(r) {}

    float lat = 0;
    float lon = 0;
    float radius = -1; // MUST be in kilometers
};

struct LatLonRadList {
    std::vector<LatLonRad> latlonrads;
};

struct UserPartition {
    enum HashOn {
        NONE,        ///< Hash always returns zero
        RANDOM,      ///< Random number

        EXCHANGEID,  ///< Hash on md5(exchange ID)
        PROVIDERID,  ///< Hash on md5(provider ID)
        IPUA,        ///< hash on md5(IP + UserAgent) (no delimiter)
    };

    struct Interval {
        Interval(int first = 0, int last = 0) : first(first), last(last) {}

        int first, last;
    };

    // Like the original, the constructor adds a 0-1 range.
    UserPartition() : includeRanges(1, Interval(0, 1)) {}

    HashOn hashOn = NONE;
    int modulus = 1;     ///< Max value of hash that's achievable

    /** A list of the hash ranges that are accepted. */
    std::vector<Interval> includeRanges;
};

struct AgentConfig {
    uint64_t externalId = 0;

    std::vector<std::string> requiredIds;

    IncludeExclude<DomainMatcher> hostFilter;
    IncludeExclude<CachedRegex> urlFilter;
    IncludeExclude<CachedRegex> languageFilter;
    IncludeExclude<CachedRegex> locationFilter;
    LatLonRadList latLongDevFilter;

    struct SegmentInfo {
        bool excludeIfNotPresent = false;
        SegmentList include;
        SegmentList exclude;

        /** What exchanges is this filter applied to?  If the exchange
            is excluded by the filter, then the filter is bypassed. */
        IncludeExclude<std::string> applyToExchanges;
    };

    std::map<std::string, SegmentInfo> segments;

    IncludeExclude<std::string> exchangeFilter;

    IncludeExclude<int> foldPositionFilter;

    struct HourOfWeekFilter {
        HourOfWeekFilter() { hourBitmap.set(); }

        std::bitset<168> hourBitmap;
    };

    HourOfWeekFilter hourOfWeekFilter;

    UserPartition userPartition;

    // Opaque per-exchange data handed to the exchange connector.
    std::map<std::string, std::shared_ptr<void> > providerData;
};

} // namespace RTBKIT

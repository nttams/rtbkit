// Port of rtbkit/common/bid_request.h (only the fields the filters use).
#pragma once

#include "segments.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace RTBKIT {

typedef std::chrono::system_clock::time_point Date;

/** Hour of the week, 0 = Sunday midnight UTC, 167 = Saturday 23h UTC. */
inline int hourOfWeek(Date date)
{
    using namespace std::chrono;
    auto day = floor<days>(date);
    int hour = int(duration_cast<hours>(date - day).count());
    return int(weekday{day}.c_encoding()) * 24 + hour;
}

/** Stand-in for Datacratic::Id: only the string form is ever needed. */
struct Id {
    Id() {}
    explicit Id(uint64_t value) : str(std::to_string(value)) {}
    explicit Id(const std::string& value) : str(value) {}

    std::string toString() const { return str; }

private:
    std::string str;
};

/** Stand-in for Datacratic::Url: just enough to get the host. */
struct Url {
    Url() {}
    Url(const std::string& s) : str(s) {}

    std::string toString() const { return str; }

    std::string host() const
    {
        size_t start = str.find("://");
        start = start == std::string::npos ? 0 : start + 3;
        size_t end = str.find_first_of("/?#", start);
        std::string authority = str.substr(
                start, end == std::string::npos ? end : end - start);

        size_t at = authority.rfind('@');
        if (at != std::string::npos) authority.erase(0, at + 1);

        size_t colon = authority.rfind(':');
        if (colon != std::string::npos
            && authority.find(']', colon) == std::string::npos)
            authority.erase(colon);

        for (char& c : authority) c = char(tolower(static_cast<unsigned char>(c)));
        return authority;
    }

    std::string str;
};

struct UserIds : public std::map<std::string, Id> {
    void add(const Id& id, const std::string& domain)
    {
        if (!insert(std::make_pair(domain, id)).second)
            throw std::runtime_error("attempt to double add id " + id.toString()
                                     + " for " + domain);
        if (domain == "prov") providerId = id;
        else if (domain == "xchg") exchangeId = id;
    }

    // These are always present
    Id exchangeId;
    Id providerId;
};

struct Location {
    std::string countryCode;
    std::string regionCode;
    std::string cityName;
    std::string postalCode;
    int dma = -1;
    int metro = -1;

    /** Return a location string with COUNTRY:REGION:CITY:POSTAL:DMA:METRO */
    std::string fullLocationString() const
    {
        return countryCode + ":" + regionCode + ":" + cityName + ":"
            + postalCode + ":" + std::to_string(dma) + ":"
            + std::to_string(metro);
    }
};

/** OpenRTB ad positions. */
enum AdPosition {
    AD_POS_UNSPECIFIED = -1,
    AD_POS_UNKNOWN = 0,
    AD_POS_ABOVE = 1,
    AD_POS_BETWEEN_DEPRECATED = 2,
    AD_POS_BELOW = 3,
    AD_POS_HEADER = 4,
    AD_POS_FOOTER = 5,
    AD_POS_SIDEBAR = 6,
    AD_POS_FULLSCREEN = 7
};

struct AdSpot {
    int position = AD_POS_UNKNOWN;
};

struct Geo {
    float lat = std::numeric_limits<float>::quiet_NaN();
    float lon = std::numeric_limits<float>::quiet_NaN();
};

struct Device {
    std::optional<Geo> geo;
};

struct BidRequest {
    Date timestamp;   // Default (epoch) means "not set".
    std::string exchange;
    std::string language;
    Location location;
    Url url;
    std::string ipAddress;
    std::string userAgent;
    UserIds userIds;
    SegmentsBySource segments;
    std::optional<Device> device;
    std::vector<AdSpot> imp;
};

} // namespace RTBKIT

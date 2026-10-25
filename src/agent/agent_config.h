// Port of rtbkit/core/agent_configuration/agent_config.h: how a bidding agent
// describes what it wants to bid on. Parsed from the original JSON format.
#pragma once

#include "common/account_key.h"
#include "common/bid_request.h"
#include "common/include_exclude.h"
#include "common/segments.h"

#include <bitset>
#include <cstdint>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace RTBKIT {

/** Thrown for invalid agent configuration JSON. */
struct AgentConfigError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};


/******************************************************************************/
/* CREATIVE                                                                   */
/******************************************************************************/

/** An image or video creative that an agent can bid with. */
struct Creative {
    enum class Type { Image, Video };

    /** A 0x0 format matches any ad spot. */
    Creative(int width = 0, int height = 0, std::string name = "", int id = -1,
             std::string dealId = "")
        : format(width, height), name(std::move(name)), id(id),
          dealId(std::move(dealId))
    {
    }

    static Creative image(int width, int height, std::string name = "",
                          int id = -1, std::string dealId = "");
    static Creative video(int width, int height, int32_t duration,
                          int64_t bitrate, std::string name = "", int id = -1,
                          std::string dealId = "");

    /** Throws AgentConfigError. */
    static Creative fromJson(const nlohmann::json& json);
    nlohmann::json toJson() const;

    Format format;

    /// Purely for information (used internally)
    std::string name;
    int id = -1;

    Type type = Type::Image;

    /// Video creative information
    int32_t duration = -1;
    int64_t bitrate = -1;

    /// Per exchange configuration, handed to the exchange connector as is.
    nlohmann::json providerConfig;

    /// Opaque per exchange data prepared by the exchange connectors.
    std::map<std::string, std::shared_ptr<void> > providerData;

    IncludeExclude<std::string> languageFilter;
    IncludeExclude<CachedRegex> locationFilter;
    IncludeExclude<std::string> exchangeFilter;

    /// Needed for the PMP filter; empty means "not a deal creative".
    std::string dealId;

    struct SegmentInfo {
        bool excludeIfNotPresent = false;
        SegmentList include;
        SegmentList exclude;

        static SegmentInfo fromJson(const nlohmann::json& json);
        nlohmann::json toJson() const;
    };

    std::map<std::string, SegmentInfo> segments;

    /// Fees are parsed and kept raw, they are not interpreted yet.
    nlohmann::json fees;

    /// Keys we do not know about.
    nlohmann::json extensions;

    /** Is the given ad spot compatible with the creative format? */
    bool compatible(const AdSpot& spot) const;

    bool isImage() const { return type == Type::Image; }
    bool isVideo() const { return type == Type::Video; }
};


/******************************************************************************/
/* LAT LONG                                                                   */
/******************************************************************************/

struct LatLonRad {
    LatLonRad() = default;
    LatLonRad(float la, float lo, float r) : lat(la), lon(lo), radius(r) {}

    float lat = 0;
    float lon = 0;
    float radius = -1; // MUST be in kilometers

    /** {"lat":..,"long":..,"radius":..}. Throws AgentConfigError. */
    static LatLonRad fromJson(const nlohmann::json& json);
    nlohmann::json toJson() const;
};

struct LatLonRadList {
    std::vector<LatLonRad> latlonrads;

    bool empty() const { return latlonrads.empty(); }
};


/******************************************************************************/
/* USER PARTITION                                                             */
/******************************************************************************/

/** Consistent partitioning of users, for A/B testing. */
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

        bool in(int val) const { return val >= first && val < last; }

        int first, last;
    };

    // Like the original, the constructor adds a 0-1 range.
    UserPartition() : includeRanges(1, Interval(0, 1)) {}

    HashOn hashOn = NONE;
    int modulus = 1;     ///< Max value of hash that's achievable

    /** A list of the hash ranges that are accepted. */
    std::vector<Interval> includeRanges;

    bool empty() const
    {
        return hashOn == NONE && modulus == 1 && includeRanges.size() == 1
            && includeRanges[0].first == 0 && includeRanges[0].last == 1;
    }

    static UserPartition fromJson(const nlohmann::json& json);
    nlohmann::json toJson() const;
};


/******************************************************************************/
/* AUGMENTATION CONFIG                                                        */
/******************************************************************************/

/** Configuration for an augmentor desired by an agent. Parsed so that
    configs stay compatible, but nothing consumes it yet. */
struct AugmentationConfig {
    std::string name;
    nlohmann::json config;
    IncludeExclude<std::string> filters;
    bool required = false;

    static AugmentationConfig fromJson(const nlohmann::json& json,
                                       const std::string& name);
    nlohmann::json toJson() const;
};


/******************************************************************************/
/* ENUMS                                                                      */
/******************************************************************************/

enum BlacklistType {
    BL_OFF,       ///< Don't blacklist
    BL_USER,      ///< Blacklist the user
    BL_USER_SITE  ///< Blacklist the user on the given site
};

enum BlacklistScope {
    BL_AGENT,      ///< Blacklist for the agent
    BL_ACCOUNT,    ///< Blacklist for an account
};

enum BidControlType {
    BC_RELAY,       ///< Relay to agent which will compute the price
    BC_RELAY_FIXED, ///< Relay to agent but bid fixed price
    BC_FIXED        ///< Bid fixed price and don't relay
};

enum BidResultFormat {
    BRF_FULL,         ///< Full message
    BRF_LIGHTWEIGHT,  ///< Lightweight message
    BRF_NONE          ///< No message
};


/******************************************************************************/
/* AGENT CONFIG                                                               */
/******************************************************************************/

/** Configuration of a bidding agent, sent to the router so it can set up the
    filtering and routing for that agent. */
struct AgentConfig {
    explicit AgentConfig(std::string name = "") : name(std::move(name)) {}

    /** Parses the original JSON format. Requires an account and at least one
        creative. Throws AgentConfigError. */
    static AgentConfig fromJson(const nlohmann::json& json);
    static AgentConfig parse(const std::string& jsonText);

    /** Inverse of fromJson: fromJson(c.toJson()).toJson() == c.toJson().
        providerData is opaque and not serialized. */
    nlohmann::json toJson() const;

    AccountKey account;   ///< Who to bill this to

    uint64_t externalId = 0;  ///< Simplifies id reconciliation with external systems

    bool external = false;    ///< Forward bid request that have this configuration
    bool test = false;        ///< Can't make real bids

    std::string roundRobinGroup;
    int roundRobinWeight = 0;

    float bidProbability = 1.0;
    float minTimeAvailableMs = 5.0;

    int maxInFlight = 100;

    std::string bidderInterface;

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

        static SegmentInfo fromJson(const nlohmann::json& json);
        nlohmann::json toJson() const;
    };

    std::map<std::string, SegmentInfo> segments;

    IncludeExclude<std::string> exchangeFilter;

    /// Ad positions, see AdPosition.
    IncludeExclude<int> foldPositionFilter;

    /// Parsed for compatibility; not used by any filter.
    SegmentInfo tagFilter;

    struct HourOfWeekFilter {
        HourOfWeekFilter() { hourBitmap.set(); }

        bool isIncluded(Date auctionDate) const
        {
            return hourBitmap[size_t(hourOfWeek(auctionDate))];
        }

        bool isDefault() const { return hourBitmap.all(); }

        /** {"hourlyBitmapSundayMidnightUtc": "<168 chars of 0/1>"} */
        static HourOfWeekFilter fromJson(const nlohmann::json& json);
        nlohmann::json toJson() const;

        std::bitset<168> hourBitmap;
    };

    HourOfWeekFilter hourOfWeekFilter;

    UserPartition userPartition;

    std::vector<Creative> creatives;

    BlacklistType blacklistType = BL_OFF;
    BlacklistScope blacklistScope = BL_ACCOUNT;
    double blacklistTime = 15.0;

    bool hasBlacklist() const
    {
        return blacklistType != BL_OFF && blacklistTime > 0.0;
    }

    BidControlType bidControlType = BC_RELAY;
    uint32_t fixedBidCpmInMicros = 0;

    std::vector<AugmentationConfig> augmentations;

    /** JSON value that is passed through with each bid. */
    nlohmann::json providerConfig;

    /// Opaque per exchange data prepared by the exchange connectors.
    std::map<std::string, std::shared_ptr<void> > providerData;

    /** List of channels for which we subscribe to post impression visit
        events. */
    SegmentList visitChannels;
    bool includeUnmatchedVisits = false;

    /** Message formats */
    BidResultFormat winFormat = BRF_FULL;
    BidResultFormat lossFormat = BRF_LIGHTWEIGHT;
    BidResultFormat errorFormat = BRF_LIGHTWEIGHT;

    nlohmann::json ext;

    /// Keys we do not know about.
    nlohmann::json extensions;

    std::string name;
};

} // namespace RTBKIT

// Port of the parts of rtbkit/common/bid_request.h (and the OpenRTB structs
// it embeds) that filtering, routing and bidding need.
#pragma once

#include "common/currency.h"
#include "common/id.h"
#include "common/segments.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace RTBKIT {

using Date = std::chrono::system_clock::time_point;

/** Hour of the week, 0 = Sunday midnight UTC, 167 = Saturday 23h UTC. */
int hourOfWeek(Date date);

/** The two id domains that every request has. */
inline constexpr const char* ID_EXCHANGE = "xchg";
inline constexpr const char* ID_PROVIDER = "prov";


/******************************************************************************/
/* URL                                                                        */
/******************************************************************************/

/** Just enough of a URL to get the host. */
struct Url {
    Url() = default;
    Url(const std::string& s) : str(s) {}
    Url(const char* s) : str(s) {}

    const std::string& toString() const { return str; }
    bool empty() const { return str.empty(); }

    /** Lower case host without user info or port. */
    std::string host() const;

    std::string str;
};


/******************************************************************************/
/* USER IDS / LOCATION                                                        */
/******************************************************************************/

struct UserIds : public std::map<std::string, Id> {
    /** Throws std::runtime_error when the domain already has an id. */
    void add(const Id& id, const std::string& domain);

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
    std::string fullLocationString() const;
};


/******************************************************************************/
/* FORMAT                                                                     */
/******************************************************************************/

/** A creative size in pixels. 0x0 on a creative means "any size". */
struct Format {
    Format() = default;
    Format(int w, int h) : width(int16_t(w)), height(int16_t(h)) {}

    bool valid() const { return width > 0 && height > 0; }

    /** "300x250". Throws std::invalid_argument on anything else. */
    static Format fromString(const std::string& s);

    /** A "300x250" string or an object with width/height (or w/h). */
    static Format fromJson(const nlohmann::json& j);

    std::string print() const;

    bool operator==(const Format&) const = default;
    auto operator<=>(const Format&) const = default;

    int16_t width = -1;
    int16_t height = -1;
};

struct FormatSet : public std::vector<Format> {
    bool compatible(const Format& format) const;
};


/******************************************************************************/
/* AD SPOT                                                                    */
/******************************************************************************/

/** OpenRTB ad positions. */
enum AdPosition : int {
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

/** Position name ("ABOVE", "below"...) or number as a string to its value.
    Throws std::invalid_argument for anything else. */
int parseAdPosition(const std::string& name);

struct Deal {
    Id id;
    Amount bidfloor;
    int auctionType = 0;   ///< 0 means "use the request's"
    std::vector<std::string> wseat;
    std::vector<std::string> wadomain;
};

/** Private marketplace information. */
struct Pmp {
    int privateAuction = 0;
    std::vector<Deal> deals;
};

struct Video {
    int width = 0;
    int height = 0;
    int minDuration = -1;
    int maxDuration = -1;
    int position = AD_POS_UNSPECIFIED;
    std::vector<std::string> mimes;
};

struct AdSpot {
    Id id;

    /** Sizes this spot accepts. Empty means any size. */
    FormatSet formats;

    /** Fold position. */
    int position = AD_POS_UNSPECIFIED;

    /** Minimum price per impression; zero when the request has no floor. */
    Amount reservePrice;

    std::string tagId;
    bool interstitial = false;
    bool isBanner = false;
    std::optional<Video> video;
    std::optional<Pmp> pmp;

    nlohmann::json ext;
};


/******************************************************************************/
/* SITE, APP, DEVICE, USER                                                    */
/******************************************************************************/

struct Publisher {
    Id id;
    std::string name;
};

struct Site {
    Id id;
    std::string name;
    std::string domain;
    std::string page;
    std::string ref;
    std::vector<std::string> categories;
    Publisher publisher;
};

struct App {
    Id id;
    std::string name;
    std::string domain;
    std::string bundle;
    std::string version;
    std::vector<std::string> categories;
    Publisher publisher;
};

struct Geo {
    float lat = std::numeric_limits<float>::quiet_NaN();
    float lon = std::numeric_limits<float>::quiet_NaN();
    std::string country;
    std::string region;
    std::string regionFips104;
    std::string city;
    std::string zip;
    std::string metro;
    int type = 0;
};

struct Device {
    std::string ip;
    std::string ipv6;
    std::string ua;
    std::string language;
    std::string os;
    std::string osVersion;
    std::string make;
    std::string model;
    std::string carrier;
    int deviceType = 0;
    int connectionType = 0;
    std::optional<Geo> geo;
};

/** One data provider's segments attached to a user. */
struct UserData {
    Id id;
    std::string name;
    std::vector<std::string> segmentIds;
    std::vector<std::string> segmentNames;
};

struct User {
    Id id;
    Id buyerUid;
    std::string gender;
    int yearOfBirth = -1;
    std::optional<Geo> geo;
    std::vector<UserData> data;
};


/******************************************************************************/
/* BID REQUEST                                                                */
/******************************************************************************/

enum class AuctionType { FirstPrice = 1, SecondPrice = 2 };

struct BidRequest {
    Id auctionId;
    AuctionType auctionType = AuctionType::SecondPrice;

    /** Time the exchange gives us to answer, in milliseconds. */
    double timeAvailableMs = 100;

    Date timestamp;   // Default (epoch) means "not set".
    bool isTest = false;

    std::string provider;
    std::string exchange;
    std::string language;
    Location location;
    Url url;
    std::string ipAddress;
    std::string userAgent;
    UserIds userIds;

    /** Targeting segments (user data, IAB categories, wseat...). */
    SegmentsBySource segments;

    /** Restrictions the creatives must respect, e.g. blocked advertisers. */
    SegmentsBySource restrictions;

    std::optional<Site> site;
    std::optional<App> app;
    std::optional<Device> device;
    std::optional<User> user;

    std::vector<AdSpot> imp;

    /** Currencies we may bid in, in order of preference. */
    std::vector<CurrencyCode> bidCurrency;

    std::vector<std::string> blockedCategories;
    std::vector<std::string> badv;

    nlohmann::json ext;
};

} // namespace RTBKIT

#include "agent/agent_config.h"

#include <algorithm>
#include <cctype>

namespace RTBKIT {

using nlohmann::json;

namespace {

[[noreturn]] void fail(const std::string& what)
{
    throw AgentConfigError(what);
}

std::string lower(std::string s)
{
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

template<typename T, typename IE = std::vector<T> >
IncludeExclude<T, IE> parseIE(const json& j, const std::string& name)
{
    try {
        return IncludeExclude<T, IE>::fromJson(j, name);
    }
    catch (const std::invalid_argument& e) {
        fail(e.what());
    }
}

SegmentList parseSegmentList(const json& j)
{
    try {
        SegmentList list = SegmentList::fromJson(j);
        list.sort();
        return list;
    }
    catch (const std::invalid_argument& e) {
        fail(e.what());
    }
}

std::string lowerString(const json& j)
{
    return lower(j.get<std::string>());
}

BidResultFormat parseResultFormat(const json& j)
{
    std::string s = lowerString(j);
    if (s == "full") return BRF_FULL;
    if (s == "lightweight") return BRF_LIGHTWEIGHT;
    if (s == "none") return BRF_NONE;
    fail("unknown BidResultFormat " + s + ": accepted full, lightweight, none");
}

} // namespace


/******************************************************************************/
/* CREATIVE                                                                   */
/******************************************************************************/

Creative Creative::image(int width, int height, std::string name, int id,
                         std::string dealId)
{
    Creative creative(width, height, std::move(name), id, std::move(dealId));
    creative.type = Type::Image;
    return creative;
}

Creative Creative::video(int width, int height, int32_t duration,
                         int64_t bitrate, std::string name, int id,
                         std::string dealId)
{
    Creative creative(width, height, std::move(name), id, std::move(dealId));
    creative.type = Type::Video;
    creative.duration = duration;
    creative.bitrate = bitrate;
    return creative;
}

Creative::SegmentInfo Creative::SegmentInfo::fromJson(const json& j)
{
    SegmentInfo info;
    for (const auto& [key, val] : j.items()) {
        if (key == "excludeIfNotPresent") info.excludeIfNotPresent = val.get<bool>();
        else if (key == "include") info.include = parseSegmentList(val);
        else if (key == "exclude") info.exclude = parseSegmentList(val);
        else fail("segmentFilter has invalid key: " + key);
    }
    return info;
}

Creative Creative::fromJson(const json& j)
{
    if (!j.is_object()) fail("a creative must be an object");

    Creative c;
    bool hasFormat = false;
    bool hasSize = false;

    try {
        for (const auto& [key, val] : j.items()) {
            if (key == "format") {
                c.format = Format::fromJson(val);
                hasFormat = true;
            }
            else if (key == "width") { c.format.width = int16_t(val.get<int>()); hasSize = true; }
            else if (key == "height") { c.format.height = int16_t(val.get<int>()); hasSize = true; }
            else if (key == "name") c.name = val.get<std::string>();
            else if (key == "id") c.id = val.get<int>();
            else if (key == "dealId") c.dealId = val.get<std::string>();
            else if (key == "providerConfig") c.providerConfig = val;
            else if (key == "languageFilter")
                c.languageFilter = parseIE<std::string>(val, "languageFilter");
            else if (key == "locationFilter")
                c.locationFilter = parseIE<CachedRegex>(val, "locationFilter");
            else if (key == "exchangeFilter")
                c.exchangeFilter = parseIE<std::string>(val, "exchangeFilter");
            else if (key == "fees") c.fees = val;
            else if (key == "segmentFilter") {
                for (const auto& [source, seg] : val.items())
                    c.segments[source] = SegmentInfo::fromJson(seg);
            }
            else if (key == "type") {
                std::string type = val.get<std::string>();
                if (type == "video") c.type = Type::Video;
                else if (type == "image") c.type = Type::Image;
                else fail("Unknown type '" + type + "'");
            }
            else if (key == "duration") c.duration = val.get<int32_t>();
            else if (key == "bitrate") c.bitrate = val.get<int64_t>();
            else c.extensions[key] = val;
        }
    }
    catch (const json::exception& e) {
        fail(std::string("invalid creative field: ") + e.what());
    }
    catch (const std::invalid_argument& e) {
        fail(e.what());
    }

    if (hasFormat && hasSize)
        fail("creative can't have both format and width/height");
    if (c.id == -1) fail("creatives require an ID to be specified");
    if (c.type == Type::Image && (c.duration != -1 || c.bitrate != -1))
        fail("bitrate or duration are not allowed for an Image creative");

    return c;
}

bool Creative::compatible(const AdSpot& spot) const
{
    return (format.width == 0 && format.height == 0)
        || spot.formats.empty()
        || spot.formats.compatible(format);
}


/******************************************************************************/
/* LAT LONG                                                                   */
/******************************************************************************/

LatLonRad LatLonRad::fromJson(const json& j)
{
    if (!j.is_object() || !j.contains("lat") || !j.contains("long")
        || !j.contains("radius"))
        fail("Error parsing LatLonRad. It should have lat, long and radius: "
             + j.dump());

    try {
        return LatLonRad(j.at("lat").get<float>(), j.at("long").get<float>(),
                         j.at("radius").get<float>());
    }
    catch (const json::exception& e) {
        fail(std::string("invalid latLongDevFilter entry: ") + e.what());
    }
}


/******************************************************************************/
/* USER PARTITION                                                             */
/******************************************************************************/

UserPartition UserPartition::fromJson(const json& j)
{
    if (!j.is_object()) fail("userPartition must be an object");

    UserPartition result;
    result.includeRanges.clear();

    try {
        for (const auto& [key, val] : j.items()) {
            if (key == "hashOn") {
                std::string name = lowerString(val);
                if (name == "null" || name == "none") result.hashOn = NONE;
                else if (name == "random") result.hashOn = RANDOM;
                else if (name == "exchangeid") result.hashOn = EXCHANGEID;
                else if (name == "providerid") result.hashOn = PROVIDERID;
                else if (name == "ipua") result.hashOn = IPUA;
                else fail("unknown hashOn value " + name);
            }
            else if (key == "modulus") {
                result.modulus = val.get<int>();
                if (result.modulus <= 0)
                    fail("userPartition modulus must be positive");
            }
            else if (key == "includeRanges") {
                if (!val.is_array()) fail("includeRanges must be an array");
                for (const auto& range : val) {
                    if (!range.is_array() || range.size() != 2)
                        fail("bad interval " + range.dump());
                    result.includeRanges.emplace_back(range[0].get<int>(),
                                                      range[1].get<int>());
                }
            }
            else fail("unknown user partition option: " + key);
        }
    }
    catch (const json::exception& e) {
        fail(std::string("invalid userPartition: ") + e.what());
    }

    return result;
}


/******************************************************************************/
/* AUGMENTATION                                                               */
/******************************************************************************/

AugmentationConfig AugmentationConfig::fromJson(const json& j,
                                                const std::string& name)
{
    if (!j.is_object()) fail("augmentation " + name + " must be an object");

    AugmentationConfig result;
    result.name = name;
    try {
        for (const auto& [key, val] : j.items()) {
            if (key == "config") result.config = val;
            else if (key == "filters")
                result.filters = parseIE<std::string>(val, "augmentor.filters");
            else if (key == "required") result.required = val.get<bool>();
            else fail("Unknown AugmentorInfo field: " + key);
        }
    }
    catch (const json::exception& e) {
        fail("invalid augmentation " + name + ": " + e.what());
    }
    return result;
}


/******************************************************************************/
/* AGENT CONFIG                                                               */
/******************************************************************************/

AgentConfig::SegmentInfo AgentConfig::SegmentInfo::fromJson(const json& j)
{
    if (!j.is_object()) fail("segmentFilter entries must be objects");

    SegmentInfo info;
    try {
        for (const auto& [key, val] : j.items()) {
            if (key == "excludeIfNotPresent")
                info.excludeIfNotPresent = val.get<bool>();
            else if (key == "include") info.include = parseSegmentList(val);
            else if (key == "exclude") info.exclude = parseSegmentList(val);
            else if (key == "applyToExchanges")
                info.applyToExchanges = parseIE<std::string>(
                        val, "segmentFilter applyToExchanges");
            else fail("segmentFilter has invalid key: " + key);
        }
    }
    catch (const json::exception& e) {
        fail(std::string("invalid segmentFilter: ") + e.what());
    }
    return info;
}

AgentConfig::HourOfWeekFilter
AgentConfig::HourOfWeekFilter::fromJson(const json& j)
{
    std::string s;
    try {
        s = j.at("hourlyBitmapSundayMidnightUtc").get<std::string>();
    }
    catch (const json::exception&) {
        fail("hourOfWeekFilter needs a hourlyBitmapSundayMidnightUtc string");
    }

    if (s.length() != 168)
        fail("Hourly bitmap string needs 168 characters");

    HourOfWeekFilter result;
    for (size_t i = 0; i < 168; ++i) {
        if (s[i] != '0' && s[i] != '1')
            fail("Hourly bitmap must contain only 0 or 1 characters");
        result.hourBitmap[i] = s[i] == '1';
    }
    return result;
}

namespace {

// Fold positions are written as numbers or as names ("ABOVE").
IncludeExclude<int> parseFoldPosition(const json& j)
{
    json normalized = j;
    if (normalized.is_object()) {
        for (auto& [key, list] : normalized.items()) {
            if (!list.is_array()) continue;
            for (auto& entry : list) {
                if (!entry.is_string()) continue;
                try {
                    entry = parseAdPosition(entry.get<std::string>());
                }
                catch (const std::invalid_argument& e) {
                    fail(e.what());
                }
            }
        }
    }
    return parseIE<int>(normalized, "foldPositionFilter");
}

} // namespace

AgentConfig AgentConfig::fromJson(const json& j)
{
    if (!j.is_object()) fail("an agent config must be an object");

    AgentConfig c;

    try {
        for (const auto& [key, val] : j.items()) {
            if (key == "account") {
                try {
                    c.account = AccountKey::fromJson(val);
                }
                catch (const std::invalid_argument& e) {
                    fail(e.what());
                }
            }
            else if (key == "name") c.name = val.get<std::string>();
            else if (key == "test") c.test = val.get<bool>();
            else if (key == "external") c.external = val.get<bool>();
            else if (key == "externalId") c.externalId = val.get<uint64_t>();
            else if (key == "requiredIds") {
                if (!val.is_array())
                    fail("requiredIds must be an array of string");
                c.requiredIds = val.get<std::vector<std::string>>();
            }
            else if (key == "roundRobin") {
                for (const auto& [rk, rv] : val.items()) {
                    if (rk == "group") c.roundRobinGroup = rv.get<std::string>();
                    else if (rk == "weight") c.roundRobinWeight = rv.get<int>();
                    else fail("roundRobin group had unknown key " + rk);
                }
            }
            else if (key == "creatives") {
                if (!val.is_array()) fail("creatives must be an array");
                for (size_t i = 0; i < val.size(); ++i) {
                    try {
                        c.creatives.push_back(Creative::fromJson(val[i]));
                    }
                    catch (const AgentConfigError& e) {
                        fail("parsing creative " + std::to_string(i) + ": "
                             + e.what());
                    }
                }
            }
            else if (key == "bidProbability") {
                c.bidProbability = val.get<float>();
                if (c.bidProbability < 0 || c.bidProbability > 1.0)
                    fail("bidProbability " + std::to_string(c.bidProbability)
                         + " not between 0 and 1");
            }
            else if (key == "minTimeAvailableMs") {
                c.minTimeAvailableMs = val.get<float>();
                if (c.minTimeAvailableMs < 0)
                    fail("minTimeAvailableMs should be not less than 0");
            }
            else if (key == "maxInFlight") {
                c.maxInFlight = val.get<int>();
                if (c.maxInFlight < 0) fail("maxInFlight has wrong value");
            }
            else if (key == "bidderInterface") c.bidderInterface = val.get<std::string>();
            else if (key == "userPartition") c.userPartition = UserPartition::fromJson(val);
            else if (key == "urlFilter") c.urlFilter = parseIE<CachedRegex>(val, key);
            else if (key == "hostFilter") c.hostFilter = parseIE<DomainMatcher>(val, key);
            else if (key == "locationFilter") c.locationFilter = parseIE<CachedRegex>(val, key);
            else if (key == "languageFilter") c.languageFilter = parseIE<CachedRegex>(val, key);
            else if (key == "exchangeFilter") c.exchangeFilter = parseIE<std::string>(val, key);
            else if (key == "latLongDevFilter") {
                if (!val.is_array()) fail("latLongDevFilter must be an array");
                for (const auto& entry : val)
                    c.latLongDevFilter.latlonrads.push_back(LatLonRad::fromJson(entry));
            }
            else if (key == "segmentFilter") {
                for (const auto& [source, seg] : val.items())
                    c.segments[source] = SegmentInfo::fromJson(seg);
            }
            else if (key == "tagFilter") c.tagFilter = SegmentInfo::fromJson(val);
            else if (key == "foldPositionFilter") c.foldPositionFilter = parseFoldPosition(val);
            else if (key == "hourOfWeekFilter")
                c.hourOfWeekFilter = HourOfWeekFilter::fromJson(val);
            else if (key == "augmentations") {
                if (!val.is_object())
                    fail("augmentations must be an object of augmentor name to config");
                for (const auto& [name, aug] : val.items())
                    c.augmentations.push_back(AugmentationConfig::fromJson(aug, name));
            }
            else if (key == "blacklist") {
                for (const auto& [bk, bv] : val.items()) {
                    if (bk == "type") {
                        std::string s = bv.is_null() ? "off" : lowerString(bv);
                        if (s == "off") c.blacklistType = BL_OFF;
                        else if (s == "user") c.blacklistType = BL_USER;
                        else if (s == "user_site") c.blacklistType = BL_USER_SITE;
                        else fail("invalid blacklist type " + s);
                    }
                    else if (bk == "time") c.blacklistTime = bv.get<double>();
                    else if (bk == "scope") {
                        std::string s = lowerString(bv);
                        if (s == "agent") c.blacklistScope = BL_AGENT;
                        else if (s == "account") c.blacklistScope = BL_ACCOUNT;
                        else fail("invalid blacklist scope " + s);
                    }
                    else fail("blacklist has invalid key: " + bk);
                }
            }
            else if (key == "visits") {
                for (const auto& [vk, vv] : val.items()) {
                    if (vk == "channels") c.visitChannels = parseSegmentList(vv);
                    else if (vk == "includeUnmatched")
                        c.includeUnmatchedVisits = vv.get<bool>();
                    else fail("visits has invalid key: " + vk);
                }
            }
            else if (key == "bidControl") {
                for (const auto& [bk, bv] : val.items()) {
                    if (bk == "type") {
                        std::string s = lowerString(bv);
                        if (s == "relay") c.bidControlType = BC_RELAY;
                        else if (s == "relay_fixed") c.bidControlType = BC_RELAY_FIXED;
                        else if (s == "fixed") c.bidControlType = BC_FIXED;
                        else fail("invalid bid control value " + s);
                    }
                    else if (bk == "fixedBidCpmInMicros")
                        c.fixedBidCpmInMicros = bv.get<uint32_t>();
                    else fail("bidControl has invalid key: " + bk);
                }
            }
            else if (key == "providerConfig") c.providerConfig = val;
            else if (key == "winFormat") c.winFormat = parseResultFormat(val);
            else if (key == "lossFormat") c.lossFormat = parseResultFormat(val);
            else if (key == "errorFormat") c.errorFormat = parseResultFormat(val);
            else if (key == "ext") c.ext = val;
            else c.extensions[key] = val;
        }
    }
    catch (const json::exception& e) {
        fail(std::string("invalid agent config: ") + e.what());
    }
    catch (const std::invalid_argument& e) {
        fail(e.what());
    }

    if (c.account.empty()) fail("each agent must have an account specified");
    if (c.creatives.empty()) fail("can't configure a agent with no creatives");

    return c;
}

AgentConfig AgentConfig::parse(const std::string& jsonText)
{
    json j;
    try {
        j = json::parse(jsonText);
    }
    catch (const json::parse_error& e) {
        fail(std::string("invalid JSON: ") + e.what());
    }
    return fromJson(j);
}

} // namespace RTBKIT

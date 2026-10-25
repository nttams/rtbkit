#include "common/openrtb_parser.h"

#include <charconv>
#include <cstdint>

#include <nlohmann/json.hpp>

namespace RTBKIT {

using nlohmann::json;

namespace {

[[noreturn]] void fail(const std::string& what)
{
    throw OpenRtbError("OpenRTB: " + what);
}

/** The value at key, or null when the key is missing or JSON null. */
const json* find(const json& obj, const char* key)
{
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return nullptr;
    return &*it;
}

const json& object(const json& j, const char* what)
{
    if (!j.is_object()) fail(std::string(what) + " must be an object");
    return j;
}

std::string str(const json& obj, const char* key)
{
    const json* v = find(obj, key);
    if (!v) return {};
    if (!v->is_string()) fail(std::string(key) + " must be a string");
    return v->get<std::string>();
}

double number(const json& obj, const char* key, double fallback)
{
    const json* v = find(obj, key);
    if (!v) return fallback;
    if (!v->is_number()) fail(std::string(key) + " must be a number");
    return v->get<double>();
}

int integer(const json& obj, const char* key, int fallback)
{
    const json* v = find(obj, key);
    if (!v) return fallback;
    if (v->is_boolean()) return v->get<bool>() ? 1 : 0;
    if (!v->is_number()) fail(std::string(key) + " must be a number");
    return int(v->get<double>());
}

std::vector<std::string> strings(const json& obj, const char* key)
{
    std::vector<std::string> result;
    const json* v = find(obj, key);
    if (!v) return result;
    if (!v->is_array()) fail(std::string(key) + " must be an array");
    for (const auto& e : *v) {
        if (!e.is_string()) fail(std::string(key) + " must contain strings");
        result.push_back(e.get<std::string>());
    }
    return result;
}

Id id(const json& obj, const char* key)
{
    const json* v = find(obj, key);
    if (!v) return Id();
    if (v->is_string()) return Id(v->get<std::string>());
    if (v->is_number_unsigned()) return Id(v->get<uint64_t>());
    if (v->is_number()) return Id(v->dump());
    fail(std::string(key) + " must be a string or a number");
}

/** Stable (across runs and platforms) 64 bit hash. */
uint64_t fnv1a(const std::string& s)
{
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    return h;
}

Amount floorPrice(const json& obj)
{
    double floor = number(obj, "bidfloor", 0.0);
    if (floor == 0.0) return Amount();

    std::string cur = str(obj, "bidfloorcur");
    CurrencyCode code = CurrencyCode::USD;
    if (!cur.empty()) {
        try {
            code = parseCurrencyCode(cur);
        }
        catch (const std::invalid_argument&) {
            fail("unsupported bidfloorcur " + cur);
        }
    }
    return Amount::fromCpm(code, floor);
}

Geo parseGeo(const json& j)
{
    object(j, "geo");
    Geo geo;
    geo.lat = float(number(j, "lat", std::numeric_limits<double>::quiet_NaN()));
    geo.lon = float(number(j, "lon", std::numeric_limits<double>::quiet_NaN()));
    geo.country = str(j, "country");
    geo.region = str(j, "region");
    geo.regionFips104 = str(j, "regionfips104");
    geo.city = str(j, "city");
    geo.zip = str(j, "zip");
    geo.metro = str(j, "metro");
    geo.type = integer(j, "type", 0);
    return geo;
}

Publisher parsePublisher(const json& j)
{
    Publisher p;
    if (const json* pub = find(j, "publisher")) {
        object(*pub, "publisher");
        p.id = id(*pub, "id");
        p.name = str(*pub, "name");
    }
    return p;
}

void addFormat(AdSpot& spot, int w, int h)
{
    if (w > 0 && h > 0) spot.formats.emplace_back(w, h);
}

AdSpot parseImpression(const json& j, BidRequest& result)
{
    object(j, "imp");

    AdSpot spot;
    spot.id = id(j, "id");
    spot.tagId = str(j, "tagid");
    spot.interstitial = integer(j, "instl", 0) == 1;
    spot.reservePrice = floorPrice(j);
    if (const json* ext = find(j, "ext")) spot.ext = *ext;

    if (const json* banner = find(j, "banner")) {
        object(*banner, "banner");
        spot.isBanner = true;

        // Pre 2.3 requests carry one w/h pair (or parallel arrays), later
        // ones a "format" array.
        const json* w = find(*banner, "w");
        const json* h = find(*banner, "h");
        if (w && h) {
            if (w->is_array() && h->is_array()) {
                if (w->size() != h->size())
                    fail("banner w and h arrays must have the same size");
                for (size_t i = 0; i < w->size(); ++i)
                    addFormat(spot, (*w)[i].get<int>(), (*h)[i].get<int>());
            }
            else if (w->is_number() && h->is_number())
                addFormat(spot, int(w->get<double>()), int(h->get<double>()));
            else fail("banner w and h must be numbers");
        }
        if (const json* formats = find(*banner, "format")) {
            if (!formats->is_array()) fail("banner format must be an array");
            for (const auto& f : *formats)
                addFormat(spot, integer(object(f, "format"), "w", 0),
                          integer(f, "h", 0));
        }

        spot.position = integer(*banner, "pos", AD_POS_UNSPECIFIED);

        // Same segment names as the original connector.
        std::vector<std::string> sdks;
        if (const json* api = find(*banner, "api")) {
            if (!api->is_array()) fail("banner api must be an array");
            for (const auto& a : *api) {
                switch (a.get<int>()) {
                case 1: sdks.push_back("VPAID 1.0"); break;
                case 2: sdks.push_back("VPAID 2.0"); break;
                case 3: sdks.push_back("MRAID"); break;
                case 4: sdks.push_back("ORMMA"); break;
                default: break;
                }
            }
        }
        if (!sdks.empty()) result.segments.addStrings("supportedSDKs", sdks);
    }
    else if (const json* v = find(j, "video")) {
        object(*v, "video");
        Video video;
        video.width = integer(*v, "w", 0);
        video.height = integer(*v, "h", 0);
        video.minDuration = integer(*v, "minduration", -1);
        video.maxDuration = integer(*v, "maxduration", -1);
        video.position = integer(*v, "pos", AD_POS_UNSPECIFIED);
        video.mimes = strings(*v, "mimes");

        if (video.maxDuration >= 0 && video.minDuration > video.maxDuration)
            fail("video maxduration can't be smaller than minduration");

        spot.position = video.position;
        addFormat(spot, video.width, video.height);
        spot.video = std::move(video);
    }

    if (const json* pmp = find(j, "pmp")) {
        object(*pmp, "pmp");
        Pmp p;
        p.privateAuction = integer(*pmp, "private_auction", 0);
        if (const json* deals = find(*pmp, "deals")) {
            if (!deals->is_array()) fail("pmp deals must be an array");
            for (const auto& d : *deals) {
                object(d, "deal");
                Deal deal;
                deal.id = id(d, "id");
                deal.bidfloor = floorPrice(d);
                deal.auctionType = integer(d, "at", 0);
                deal.wseat = strings(d, "wseat");
                deal.wadomain = strings(d, "wadomain");
                p.deals.push_back(std::move(deal));
            }
        }
        spot.pmp = std::move(p);
    }

    return spot;
}

void parseSite(const json& j, BidRequest& result)
{
    object(j, "site");
    Site site;
    site.id = id(j, "id");
    site.name = str(j, "name");
    site.domain = str(j, "domain");
    site.page = str(j, "page");
    site.ref = str(j, "ref");
    site.categories = strings(j, "cat");
    site.publisher = parsePublisher(j);

    if (!site.page.empty()) result.url = Url(site.page);
    else if (site.id) result.url = Url("http://" + site.id.toString() + ".siteid/");

    for (const auto& cat : site.categories)
        result.segments.add("iab-categories", cat);

    result.site = std::move(site);
}

void parseApp(const json& j, BidRequest& result)
{
    object(j, "app");
    App app;
    app.id = id(j, "id");
    app.name = str(j, "name");
    app.domain = str(j, "domain");
    app.bundle = str(j, "bundle");
    app.version = str(j, "ver");
    app.categories = strings(j, "cat");
    app.publisher = parsePublisher(j);

    if (!app.bundle.empty()) result.url = Url(app.bundle);
    else if (app.id) result.url = Url("http://" + app.id.toString() + ".appid/");

    for (const auto& cat : app.categories)
        result.segments.add("iab-categories", cat);

    result.app = std::move(app);
}

void parseDevice(const json& j, BidRequest& result)
{
    object(j, "device");
    Device device;
    device.ip = str(j, "ip");
    device.ipv6 = str(j, "ipv6");
    device.ua = str(j, "ua");
    device.language = str(j, "language");
    device.os = str(j, "os");
    device.osVersion = str(j, "osv");
    device.make = str(j, "make");
    device.model = str(j, "model");
    device.carrier = str(j, "carrier");
    device.deviceType = integer(j, "devicetype", 0);
    device.connectionType = integer(j, "connectiontype", 0);
    if (const json* geo = find(j, "geo")) device.geo = parseGeo(*geo);

    result.language = device.language;
    result.userAgent = device.ua;
    result.ipAddress = !device.ip.empty() ? device.ip : device.ipv6;

    if (device.geo) {
        const Geo& g = *device.geo;
        Location& l = result.location;
        l.countryCode = g.country;
        l.regionCode = !g.region.empty() ? g.region : g.regionFips104;
        l.cityName = g.city;
        l.postalCode = g.zip;

        int metro = 0;
        auto [end, ec] = std::from_chars(g.metro.data(),
                                         g.metro.data() + g.metro.size(), metro);
        if (ec == std::errc() && end == g.metro.data() + g.metro.size())
            l.metro = metro;
    }

    result.device = std::move(device);
}

std::string idString(const json& j)
{
    if (j.is_string()) return j.get<std::string>();
    if (j.is_number()) return j.dump();
    fail("segment id must be a string or a number");
}

void parseUser(const json& j, BidRequest& result)
{
    object(j, "user");
    User user;
    user.id = id(j, "id");
    user.buyerUid = id(j, "buyeruid");
    user.gender = str(j, "gender");
    user.yearOfBirth = integer(j, "yob", -1);
    if (const json* geo = find(j, "geo")) user.geo = parseGeo(*geo);

    if (const json* data = find(j, "data")) {
        if (!data->is_array()) fail("user data must be an array");
        for (const auto& d : *data) {
            object(d, "data");
            UserData ud;
            ud.id = id(d, "id");
            ud.name = str(d, "name");

            // Key by the provider id, falling back to its name.
            std::string key = ud.id ? ud.id.toString() : ud.name;
            if (const json* segs = find(d, "segment")) {
                if (!segs->is_array()) fail("data segment must be an array");
                std::vector<std::string> values;
                for (const auto& s : *segs) {
                    object(s, "segment");
                    if (const json* sid = find(s, "id")) {
                        ud.segmentIds.push_back(idString(*sid));
                        values.push_back(ud.segmentIds.back());
                    }
                    else if (!str(s, "name").empty()) {
                        ud.segmentNames.push_back(str(s, "name"));
                        values.push_back(ud.segmentNames.back());
                    }
                }
                result.segments.addStrings(key, values);
            }
            user.data.push_back(std::move(ud));
        }
    }

    if (user.id) result.userIds.add(user.id, ID_EXCHANGE);

    if (user.buyerUid) result.userIds.add(user.buyerUid, ID_PROVIDER);
    else if (user.id) result.userIds.add(user.id, ID_PROVIDER);

    if (user.geo) {
        const Geo& g = *user.geo;
        Location& l = result.location;
        if (l.countryCode.empty()) l.countryCode = g.country;
        if (l.regionCode.empty()) l.regionCode = g.region;
        if (l.cityName.empty()) l.cityName = g.city;
        if (l.postalCode.empty()) l.postalCode = g.zip;
    }

    result.user = std::move(user);
}

} // namespace

BidRequest parseOpenRtbBidRequest(const json& req, const OpenRtbOptions& options)
{
    try {
        object(req, "bid request");

        BidRequest result;
        result.auctionId = id(req, "id");
        if (!result.auctionId) fail("bid request has no id");

        result.auctionType = integer(req, "at", 2) == 1
            ? AuctionType::FirstPrice : AuctionType::SecondPrice;
        result.timeAvailableMs = number(req, "tmax", result.timeAvailableMs);
        result.timestamp = options.now.value_or(std::chrono::system_clock::now());
        result.provider = options.provider;
        result.exchange = options.exchange.empty() ? options.provider
                                                   : options.exchange;

        const json* imps = find(req, "imp");
        if (!imps || !imps->is_array() || imps->empty())
            fail("bid request needs a non empty imp array");
        for (const auto& imp : *imps)
            result.imp.push_back(parseImpression(imp, result));

        const json* site = find(req, "site");
        const json* app = find(req, "app");
        if (site && app) fail("can't have site and app");
        if (site) parseSite(*site, result);
        if (app) parseApp(*app, result);

        if (const json* device = find(req, "device")) parseDevice(*device, result);
        if (const json* user = find(req, "user")) parseUser(*user, result);

        // The provider id identifies the user for partitioning even when the
        // exchange gave us nothing: fall back to a hash of ip + user agent.
        if (!result.userIds.contains(ID_PROVIDER)) {
            if (result.device && !result.device->ip.empty()
                && !result.device->ua.empty()) {
                result.userIds.add(
                    Id(fnv1a(result.device->ip + result.device->ua)), ID_PROVIDER);
            }
            else result.userIds.add(Id(uint64_t(0)), ID_PROVIDER);
        }

        for (const auto& cur : strings(req, "cur")) {
            try {
                result.bidCurrency.push_back(parseCurrencyCode(cur));
            }
            catch (const std::invalid_argument&) {
                fail("unsupported currency " + cur);
            }
        }
        if (result.bidCurrency.empty())
            result.bidCurrency.push_back(CurrencyCode::USD);

        result.blockedCategories = strings(req, "bcat");
        result.badv = strings(req, "badv");
        if (!result.badv.empty())
            result.restrictions.addStrings("badv", result.badv);
        result.segments.addStrings("openrtb-wseat", strings(req, "wseat"));
        if (const json* ext = find(req, "ext")) result.ext = *ext;

        return result;
    }
    catch (const json::exception& e) {
        fail(std::string("bad field type: ") + e.what());
    }
}

BidRequest parseOpenRtbBidRequestText(std::string_view body,
                                      const OpenRtbOptions& options)
{
    json parsed;
    try {
        parsed = json::parse(body);
    }
    catch (const json::parse_error& e) {
        fail(std::string("invalid JSON: ") + e.what());
    }
    return parseOpenRtbBidRequest(parsed, options);
}

} // namespace RTBKIT

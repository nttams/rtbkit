#include "common/bid_request_json.h"

#include <cmath>

namespace RTBKIT {

using nlohmann::json;

int64_t dateToMs(Date date)
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(date.time_since_epoch()).count();
}

Date dateFromMs(int64_t ms)
{
    using namespace std::chrono;
    return Date(duration_cast<Date::duration>(milliseconds(ms)));
}

namespace {

// Writers. Empty / default members are left out to keep messages small.

void put(json& j, const char* key, const std::string& v)
{
    if (!v.empty()) j[key] = v;
}

void put(json& j, const char* key, const Id& v)
{
    if (!v.empty()) j[key] = v.toString();
}

void put(json& j, const char* key, const std::vector<std::string>& v)
{
    if (!v.empty()) j[key] = v;
}

json geoJson(const Geo& g)
{
    json j = json::object();
    // NaN is written as null.
    j["lat"] = std::isnan(g.lat) ? json(nullptr) : json(g.lat);
    j["lon"] = std::isnan(g.lon) ? json(nullptr) : json(g.lon);
    put(j, "country", g.country);
    put(j, "region", g.region);
    put(j, "regionFips104", g.regionFips104);
    put(j, "city", g.city);
    put(j, "zip", g.zip);
    put(j, "metro", g.metro);
    j["type"] = g.type;
    return j;
}

json publisherJson(const Publisher& p)
{
    json j = json::object();
    put(j, "id", p.id);
    put(j, "name", p.name);
    return j;
}

json dealJson(const Deal& d)
{
    json j = json::object();
    put(j, "id", d.id);
    j["bidfloor"] = d.bidfloor;
    j["auctionType"] = d.auctionType;
    put(j, "wseat", d.wseat);
    put(j, "wadomain", d.wadomain);
    return j;
}

json spotJson(const AdSpot& s)
{
    json j = json::object();
    put(j, "id", s.id);
    json formats = json::array();
    for (const auto& f : s.formats) formats.push_back(f.print());
    j["formats"] = formats;
    j["position"] = s.position;
    j["reservePrice"] = s.reservePrice;
    put(j, "tagId", s.tagId);
    j["interstitial"] = s.interstitial;
    j["isBanner"] = s.isBanner;
    if (s.video) {
        const Video& v = *s.video;
        j["video"] = json{{"width", v.width}, {"height", v.height},
                          {"minDuration", v.minDuration},
                          {"maxDuration", v.maxDuration},
                          {"position", v.position}, {"mimes", v.mimes}};
    }
    if (s.pmp) {
        json deals = json::array();
        for (const auto& d : s.pmp->deals) deals.push_back(dealJson(d));
        j["pmp"] = json{{"privateAuction", s.pmp->privateAuction}, {"deals", deals}};
    }
    if (!s.ext.is_null()) j["ext"] = s.ext;
    return j;
}

json siteJson(const Site& s)
{
    json j = json::object();
    put(j, "id", s.id);
    put(j, "name", s.name);
    put(j, "domain", s.domain);
    put(j, "page", s.page);
    put(j, "ref", s.ref);
    put(j, "categories", s.categories);
    j["publisher"] = publisherJson(s.publisher);
    return j;
}

json appJson(const App& a)
{
    json j = json::object();
    put(j, "id", a.id);
    put(j, "name", a.name);
    put(j, "domain", a.domain);
    put(j, "bundle", a.bundle);
    put(j, "version", a.version);
    put(j, "categories", a.categories);
    j["publisher"] = publisherJson(a.publisher);
    return j;
}

json deviceJson(const Device& d)
{
    json j = json::object();
    put(j, "ip", d.ip);
    put(j, "ipv6", d.ipv6);
    put(j, "ua", d.ua);
    put(j, "language", d.language);
    put(j, "os", d.os);
    put(j, "osVersion", d.osVersion);
    put(j, "make", d.make);
    put(j, "model", d.model);
    put(j, "carrier", d.carrier);
    j["deviceType"] = d.deviceType;
    j["connectionType"] = d.connectionType;
    if (d.geo) j["geo"] = geoJson(*d.geo);
    return j;
}

json userJson(const User& u)
{
    json j = json::object();
    put(j, "id", u.id);
    put(j, "buyerUid", u.buyerUid);
    put(j, "gender", u.gender);
    j["yearOfBirth"] = u.yearOfBirth;
    if (u.geo) j["geo"] = geoJson(*u.geo);
    json data = json::array();
    for (const auto& d : u.data) {
        json dj = json::object();
        put(dj, "id", d.id);
        put(dj, "name", d.name);
        dj["segmentIds"] = d.segmentIds;
        dj["segmentNames"] = d.segmentNames;
        data.push_back(dj);
    }
    j["data"] = data;
    return j;
}

// Readers.

std::string str(const json& j, const char* key)
{
    auto it = j.find(key);
    return it == j.end() || it->is_null() ? std::string() : it->get<std::string>();
}

Id id(const json& j, const char* key) { return Id(str(j, key)); }

std::vector<std::string> strings(const json& j, const char* key)
{
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return {};
    return it->get<std::vector<std::string>>();
}

template<typename T>
T num(const json& j, const char* key, T fallback)
{
    auto it = j.find(key);
    return it == j.end() || it->is_null() ? fallback : it->get<T>();
}

float nanOrFloat(const json& j, const char* key)
{
    auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return std::numeric_limits<float>::quiet_NaN();
    return it->get<float>();
}

Amount amount(const json& j, const char* key)
{
    auto it = j.find(key);
    return it == j.end() ? Amount() : it->get<Amount>();
}

Geo geoFrom(const json& j)
{
    Geo g;
    g.lat = nanOrFloat(j, "lat");
    g.lon = nanOrFloat(j, "lon");
    g.country = str(j, "country");
    g.region = str(j, "region");
    g.regionFips104 = str(j, "regionFips104");
    g.city = str(j, "city");
    g.zip = str(j, "zip");
    g.metro = str(j, "metro");
    g.type = num(j, "type", 0);
    return g;
}

Publisher publisherFrom(const json& j)
{
    Publisher p;
    if (j.is_object()) { p.id = id(j, "id"); p.name = str(j, "name"); }
    return p;
}

AdSpot spotFrom(const json& j)
{
    AdSpot s;
    s.id = id(j, "id");
    if (j.contains("formats"))
        for (const auto& f : j.at("formats")) s.formats.push_back(Format::fromJson(f));
    s.position = num(j, "position", int(AD_POS_UNSPECIFIED));
    s.reservePrice = amount(j, "reservePrice");
    s.tagId = str(j, "tagId");
    s.interstitial = num(j, "interstitial", false);
    s.isBanner = num(j, "isBanner", false);
    if (j.contains("video")) {
        const json& v = j.at("video");
        Video video;
        video.width = num(v, "width", 0);
        video.height = num(v, "height", 0);
        video.minDuration = num(v, "minDuration", -1);
        video.maxDuration = num(v, "maxDuration", -1);
        video.position = num(v, "position", int(AD_POS_UNSPECIFIED));
        video.mimes = strings(v, "mimes");
        s.video = video;
    }
    if (j.contains("pmp")) {
        const json& p = j.at("pmp");
        Pmp pmp;
        pmp.privateAuction = num(p, "privateAuction", 0);
        if (p.contains("deals")) {
            for (const auto& dj : p.at("deals")) {
                Deal d;
                d.id = id(dj, "id");
                d.bidfloor = amount(dj, "bidfloor");
                d.auctionType = num(dj, "auctionType", 0);
                d.wseat = strings(dj, "wseat");
                d.wadomain = strings(dj, "wadomain");
                pmp.deals.push_back(std::move(d));
            }
        }
        s.pmp = std::move(pmp);
    }
    if (j.contains("ext")) s.ext = j.at("ext");
    return s;
}

} // namespace

json toJson(const BidRequest& r)
{
    json j = json::object();
    j["auctionId"] = r.auctionId.toString();
    j["auctionType"] = int(r.auctionType);
    j["timeAvailableMs"] = r.timeAvailableMs;
    j["timestamp"] = dateToMs(r.timestamp);
    j["isTest"] = r.isTest;
    put(j, "provider", r.provider);
    put(j, "exchange", r.exchange);
    put(j, "language", r.language);

    json loc = json::object();
    put(loc, "countryCode", r.location.countryCode);
    put(loc, "regionCode", r.location.regionCode);
    put(loc, "cityName", r.location.cityName);
    put(loc, "postalCode", r.location.postalCode);
    loc["dma"] = r.location.dma;
    loc["metro"] = r.location.metro;
    j["location"] = loc;

    put(j, "url", r.url.toString());
    put(j, "ipAddress", r.ipAddress);
    put(j, "userAgent", r.userAgent);

    json ids = json::object();
    for (const auto& [domain, value] : r.userIds) ids[domain] = value.toString();
    j["userIds"] = ids;

    j["segments"] = r.segments.toJson();
    j["restrictions"] = r.restrictions.toJson();

    if (r.site) j["site"] = siteJson(*r.site);
    if (r.app) j["app"] = appJson(*r.app);
    if (r.device) j["device"] = deviceJson(*r.device);
    if (r.user) j["user"] = userJson(*r.user);

    json imps = json::array();
    for (const auto& s : r.imp) imps.push_back(spotJson(s));
    j["imp"] = imps;

    json cur = json::array();
    for (CurrencyCode c : r.bidCurrency) cur.push_back(RTBKIT::toString(c));
    j["bidCurrency"] = cur;

    put(j, "blockedCategories", r.blockedCategories);
    put(j, "badv", r.badv);
    if (!r.ext.is_null()) j["ext"] = r.ext;
    return j;
}

BidRequest bidRequestFromJson(const json& j)
{
    if (!j.is_object()) throw std::runtime_error("bid request JSON must be an object");

    try {
        BidRequest r;
        r.auctionId = id(j, "auctionId");
        r.auctionType = AuctionType(num(j, "auctionType", int(AuctionType::SecondPrice)));
        r.timeAvailableMs = num(j, "timeAvailableMs", 100.0);
        r.timestamp = dateFromMs(num<int64_t>(j, "timestamp", 0));
        r.isTest = num(j, "isTest", false);
        r.provider = str(j, "provider");
        r.exchange = str(j, "exchange");
        r.language = str(j, "language");

        if (j.contains("location")) {
            const json& l = j.at("location");
            r.location.countryCode = str(l, "countryCode");
            r.location.regionCode = str(l, "regionCode");
            r.location.cityName = str(l, "cityName");
            r.location.postalCode = str(l, "postalCode");
            r.location.dma = num(l, "dma", -1);
            r.location.metro = num(l, "metro", -1);
        }

        r.url = Url(str(j, "url"));
        r.ipAddress = str(j, "ipAddress");
        r.userAgent = str(j, "userAgent");

        if (j.contains("userIds"))
            for (const auto& [domain, value] : j.at("userIds").items())
                r.userIds.add(Id(value.get<std::string>()), domain);

        if (j.contains("segments")) r.segments = SegmentsBySource::fromJson(j.at("segments"));
        if (j.contains("restrictions"))
            r.restrictions = SegmentsBySource::fromJson(j.at("restrictions"));

        if (j.contains("site")) {
            const json& s = j.at("site");
            Site site;
            site.id = id(s, "id");
            site.name = str(s, "name");
            site.domain = str(s, "domain");
            site.page = str(s, "page");
            site.ref = str(s, "ref");
            site.categories = strings(s, "categories");
            if (s.contains("publisher")) site.publisher = publisherFrom(s.at("publisher"));
            r.site = std::move(site);
        }
        if (j.contains("app")) {
            const json& a = j.at("app");
            App app;
            app.id = id(a, "id");
            app.name = str(a, "name");
            app.domain = str(a, "domain");
            app.bundle = str(a, "bundle");
            app.version = str(a, "version");
            app.categories = strings(a, "categories");
            if (a.contains("publisher")) app.publisher = publisherFrom(a.at("publisher"));
            r.app = std::move(app);
        }
        if (j.contains("device")) {
            const json& d = j.at("device");
            Device dev;
            dev.ip = str(d, "ip");
            dev.ipv6 = str(d, "ipv6");
            dev.ua = str(d, "ua");
            dev.language = str(d, "language");
            dev.os = str(d, "os");
            dev.osVersion = str(d, "osVersion");
            dev.make = str(d, "make");
            dev.model = str(d, "model");
            dev.carrier = str(d, "carrier");
            dev.deviceType = num(d, "deviceType", 0);
            dev.connectionType = num(d, "connectionType", 0);
            if (d.contains("geo")) dev.geo = geoFrom(d.at("geo"));
            r.device = std::move(dev);
        }
        if (j.contains("user")) {
            const json& u = j.at("user");
            User user;
            user.id = id(u, "id");
            user.buyerUid = id(u, "buyerUid");
            user.gender = str(u, "gender");
            user.yearOfBirth = num(u, "yearOfBirth", -1);
            if (u.contains("geo")) user.geo = geoFrom(u.at("geo"));
            if (u.contains("data")) {
                for (const auto& dj : u.at("data")) {
                    UserData d;
                    d.id = id(dj, "id");
                    d.name = str(dj, "name");
                    d.segmentIds = strings(dj, "segmentIds");
                    d.segmentNames = strings(dj, "segmentNames");
                    user.data.push_back(std::move(d));
                }
            }
            r.user = std::move(user);
        }

        if (j.contains("imp")) {
            if (!j.at("imp").is_array()) throw std::runtime_error("imp must be an array");
            for (const auto& s : j.at("imp")) r.imp.push_back(spotFrom(s));
        }

        if (j.contains("bidCurrency")) {
            if (!j.at("bidCurrency").is_array())
                throw std::runtime_error("bidCurrency must be an array");
            for (const auto& c : j.at("bidCurrency"))
                r.bidCurrency.push_back(parseCurrencyCode(c.get<std::string>()));
        }

        r.blockedCategories = strings(j, "blockedCategories");
        r.badv = strings(j, "badv");
        if (j.contains("ext")) r.ext = j.at("ext");
        return r;
    }
    catch (const json::exception& e) {
        throw std::runtime_error(std::string("invalid bid request JSON: ") + e.what());
    }
    catch (const std::invalid_argument& e) {
        throw std::runtime_error(std::string("invalid bid request JSON: ") + e.what());
    }
}

void to_json(json& j, const BidRequest& request) { j = toJson(request); }

void from_json(const json& j, BidRequest& request) { request = bidRequestFromJson(j); }

} // namespace RTBKIT

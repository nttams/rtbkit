// Demo: load campaigns and bid requests from JSON, run the static filters and
// print the matching campaigns plus the reject reason for the others.
//
// usage: demo campaigns.json bid_requests.json

#include "agent_config.h"
#include "filter_pool.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>

using json = nlohmann::json;
using namespace RTBKIT;

namespace {

/******************************************************************************/
/* DEMO EXCHANGE CONNECTOR                                                    */
/******************************************************************************/

/** Per campaign data kept in AgentConfig::providerData["demo"]. */
struct DemoProviderData {
    bool pre = true;
    bool post = true;
};

struct DemoExchangeConnector : public ExchangeConnector
{
    std::string exchangeName() const { return "demo"; }

    bool bidRequestPreFilter(
            const BidRequest&, const AgentConfig&, const void* data) const
    {
        return static_cast<const DemoProviderData*>(data)->pre;
    }

    bool bidRequestPostFilter(
            const BidRequest&, const AgentConfig&, const void* data) const
    {
        return static_cast<const DemoProviderData*>(data)->post;
    }
};


/******************************************************************************/
/* JSON PARSING                                                               */
/******************************************************************************/

json readJson(const std::string& path)
{
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);
    return json::parse(in);
}

SegmentList parseSegments(const json& j)
{
    SegmentList list;
    for (const auto& v : j) {
        if (v.is_number_integer()) list.add(v.get<int>());
        else list.add(v.get<std::string>());
    }
    list.sort();
    return list;
}

int parsePosition(const json& j)
{
    if (j.is_number_integer()) return j.get<int>();

    static const std::map<std::string, int> names = {
        { "UNSPECIFIED", AD_POS_UNSPECIFIED }, { "UNKNOWN", AD_POS_UNKNOWN },
        { "ABOVE", AD_POS_ABOVE }, { "BELOW", AD_POS_BELOW },
        { "HEADER", AD_POS_HEADER }, { "FOOTER", AD_POS_FOOTER },
        { "SIDEBAR", AD_POS_SIDEBAR }, { "FULLSCREEN", AD_POS_FULLSCREEN } };
    return names.at(j.get<std::string>());
}

template<typename T, typename Parse>
IncludeExclude<T> parseIE(const json& j, Parse parse)
{
    IncludeExclude<T> ie;
    if (j.contains("include"))
        for (const auto& v : j["include"]) ie.include.push_back(parse(v));
    if (j.contains("exclude"))
        for (const auto& v : j["exclude"]) ie.exclude.push_back(parse(v));
    return ie;
}

auto asString = [] (const json& v) { return v.get<std::string>(); };

std::shared_ptr<AgentConfig> parseCampaign(const json& j)
{
    auto config = std::make_shared<AgentConfig>();

    config->externalId = j.value("externalId", 0);

    if (j.contains("requiredIds"))
        config->requiredIds = j["requiredIds"].get<std::vector<std::string>>();

    if (j.contains("hostFilter"))
        config->hostFilter = parseIE<DomainMatcher>(j["hostFilter"],
                [] (const json& v) { return DomainMatcher(v.get<std::string>()); });

    auto regex = [] (const json& v) { return CachedRegex(v.get<std::string>()); };
    if (j.contains("urlFilter"))
        config->urlFilter = parseIE<CachedRegex>(j["urlFilter"], regex);
    if (j.contains("languageFilter"))
        config->languageFilter = parseIE<CachedRegex>(j["languageFilter"], regex);
    if (j.contains("locationFilter"))
        config->locationFilter = parseIE<CachedRegex>(j["locationFilter"], regex);

    if (j.contains("exchangeFilter"))
        config->exchangeFilter = parseIE<std::string>(j["exchangeFilter"], asString);

    if (j.contains("foldPositionFilter"))
        config->foldPositionFilter = parseIE<int>(j["foldPositionFilter"], parsePosition);

    if (j.contains("latLongDevFilter"))
        for (const auto& v : j["latLongDevFilter"])
            config->latLongDevFilter.latlonrads.emplace_back(
                    v["lat"].get<float>(), v["long"].get<float>(),
                    v["radius"].get<float>());

    if (j.contains("segments")) {
        for (const auto& [name, v] : j["segments"].items()) {
            AgentConfig::SegmentInfo info;
            info.excludeIfNotPresent = v.value("excludeIfNotPresent", false);
            if (v.contains("include")) info.include = parseSegments(v["include"]);
            if (v.contains("exclude")) info.exclude = parseSegments(v["exclude"]);
            if (v.contains("applyToExchanges"))
                info.applyToExchanges =
                    parseIE<std::string>(v["applyToExchanges"], asString);
            config->segments[name] = info;
        }
    }

    if (j.contains("hourOfWeekFilter")) {
        std::string bits = j["hourOfWeekFilter"]["hourlyBitmapSundayMidnightUtc"];
        if (bits.size() != 168)
            throw std::runtime_error("hourly bitmap needs 168 characters");
        for (size_t i = 0; i < 168; ++i) config->hourOfWeekFilter.hourBitmap[i] = bits[i] != '0';
    }

    if (j.contains("userPartition")) {
        const auto& p = j["userPartition"];
        static const std::map<std::string, UserPartition::HashOn> hashOn = {
            { "NONE", UserPartition::NONE }, { "RANDOM", UserPartition::RANDOM },
            { "EXCHANGEID", UserPartition::EXCHANGEID },
            { "PROVIDERID", UserPartition::PROVIDERID },
            { "IPUA", UserPartition::IPUA } };
        config->userPartition.hashOn = hashOn.at(p["hashOn"].get<std::string>());
        config->userPartition.modulus = p["modulus"].get<int>();
        config->userPartition.includeRanges.clear();
        for (const auto& r : p["includeRanges"])
            config->userPartition.includeRanges.emplace_back(r[0].get<int>(), r[1].get<int>());
    }

    if (j.contains("providerData")) {
        for (const auto& [name, v] : j["providerData"].items()) {
            auto data = std::make_shared<DemoProviderData>();
            data->pre = v.value("pre", true);
            data->post = v.value("post", true);
            config->providerData[name] = data;
        }
    }

    return config;
}

Date parseTimestamp(const std::string& s)
{
    using namespace std::chrono;
    int y, mo, d, h = 0, mi = 0, sec = 0;
    if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%dZ", &y, &mo, &d, &h, &mi, &sec) < 3)
        throw std::runtime_error("bad timestamp (want YYYY-MM-DDTHH:MM:SSZ): " + s);

    sys_days date = year{y} / month(mo) / day(d);
    return date + hours(h) + minutes(mi) + seconds(sec);
}

BidRequest parseBidRequest(const json& j)
{
    BidRequest br;

    br.exchange = j.value("exchange", "");
    br.language = j.value("language", "");
    br.url = Url(j.value("url", ""));
    br.ipAddress = j.value("ipAddress", "");
    br.userAgent = j.value("userAgent", "");
    if (j.contains("timestamp")) br.timestamp = parseTimestamp(j["timestamp"]);

    if (j.contains("location")) {
        const auto& l = j["location"];
        br.location.countryCode = l.value("countryCode", "");
        br.location.regionCode = l.value("regionCode", "");
        br.location.cityName = l.value("cityName", "");
        br.location.postalCode = l.value("postalCode", "");
        br.location.dma = l.value("dma", -1);
        br.location.metro = l.value("metro", -1);
    }

    if (j.contains("userIds"))
        for (const auto& [domain, id] : j["userIds"].items())
            br.userIds.add(Id(id.get<std::string>()), domain);

    if (j.contains("segments"))
        for (const auto& [name, v] : j["segments"].items())
            br.segments[name] = std::make_shared<SegmentList>(parseSegments(v));

    if (j.contains("device") && j["device"].contains("geo")) {
        br.device.emplace();
        br.device->geo.emplace();
        br.device->geo->lat = j["device"]["geo"]["lat"].get<float>();
        br.device->geo->lon = j["device"]["geo"]["lon"].get<float>();
    }

    if (j.contains("imp")) {
        for (const auto& v : j["imp"]) {
            AdSpot spot;
            spot.position = parsePosition(v.value("position", json(0)));
            br.imp.push_back(spot);
        }
    }

    return br;
}

} // namespace anonymous


int main(int argc, char** argv)
{
    if (argc != 3) {
        std::cerr << "usage: " << argv[0] << " campaigns.json bid_requests.json\n";
        return 2;
    }

    try {
        json campaigns = readJson(argv[1]);
        json requests = readJson(argv[2]);

        FilterPool pool;
        pool.initWithDefaultFilters();

        for (const auto& c : campaigns)
            pool.addConfig(c.at("name").get<std::string>(), parseCampaign(c));

        DemoExchangeConnector connector;

        size_t n = 0;
        for (const auto& r : requests) {
            BidRequest br = parseBidRequest(r);
            auto result = pool.filter(br, &connector);

            std::cout << "request " << ++n << " (" << r.value("name", "") << ")\n";

            std::cout << "  matched:";
            if (result.matched.empty()) std::cout << " (none)";
            for (const auto& name : result.matched) std::cout << " " << name;
            std::cout << "\n";

            for (const auto& [name, filter] : result.rejected)
                std::cout << "  rejected: " << name << " by " << filter << "\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}

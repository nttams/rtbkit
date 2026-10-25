#include "common/bid_request.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace RTBKIT {

int hourOfWeek(Date date)
{
    using namespace std::chrono;
    auto day = floor<days>(date);
    int hour = int(duration_cast<hours>(date - day).count());
    return int(weekday{day}.c_encoding()) * 24 + hour;
}

std::string Url::host() const
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

    for (char& c : authority) c = char(std::tolower(static_cast<unsigned char>(c)));
    return authority;
}

void UserIds::add(const Id& id, const std::string& domain)
{
    if (!insert(std::make_pair(domain, id)).second)
        throw std::runtime_error("attempt to double add id " + id.toString()
                                 + " for " + domain);
    if (domain == ID_PROVIDER) providerId = id;
    else if (domain == ID_EXCHANGE) exchangeId = id;
}

std::string Location::fullLocationString() const
{
    return countryCode + ":" + regionCode + ":" + cityName + ":"
        + postalCode + ":" + std::to_string(dma) + ":" + std::to_string(metro);
}

Format Format::fromString(const std::string& s)
{
    int width = 0, height = 0, nchars = -1;
    int res = std::sscanf(s.c_str(), "%dx%d%n", &width, &height, &nchars);
    if (res < 2 || nchars < 0 || size_t(nchars) != s.size())
        throw std::invalid_argument("couldn't parse format string " + s);
    return Format(width, height);
}

Format Format::fromJson(const nlohmann::json& j)
{
    if (j.is_string()) return fromString(j.get<std::string>());

    if (j.is_object()) {
        auto get = [&](const char* longKey, const char* shortKey) {
            if (j.contains(longKey)) return j.at(longKey).get<int>();
            return j.at(shortKey).get<int>();
        };
        try {
            return Format(get("width", "w"), get("height", "h"));
        }
        catch (const nlohmann::json::exception&) {
            throw std::invalid_argument("format needs width and height: " + j.dump());
        }
    }

    throw std::invalid_argument("couldn't parse format " + j.dump());
}

std::string Format::print() const
{
    return std::to_string(width) + "x" + std::to_string(height);
}

bool FormatSet::compatible(const Format& format) const
{
    return std::find(begin(), end(), format) != end();
}

int parseAdPosition(const std::string& name)
{
    std::string upper;
    for (char c : name) upper += char(std::toupper(static_cast<unsigned char>(c)));

    static const std::pair<const char*, int> names[] = {
        { "UNSPECIFIED", AD_POS_UNSPECIFIED },
        { "UNKNOWN", AD_POS_UNKNOWN },
        { "ABOVE", AD_POS_ABOVE },
        { "BETWEEN_DEPRECATED", AD_POS_BETWEEN_DEPRECATED },
        { "BELOW", AD_POS_BELOW },
        { "HEADER", AD_POS_HEADER },
        { "FOOTER", AD_POS_FOOTER },
        { "SIDEBAR", AD_POS_SIDEBAR },
        { "FULLSCREEN", AD_POS_FULLSCREEN },
    };
    for (const auto& [n, value] : names)
        if (upper == n) return value;

    size_t used = 0;
    try {
        int value = std::stoi(name, &used);
        if (used == name.size() && value >= -1 && value <= 7) return value;
    }
    catch (const std::logic_error&) {
    }
    throw std::invalid_argument("unknown ad position " + name);
}

} // namespace RTBKIT

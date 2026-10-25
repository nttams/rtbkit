#include "exchange/mock_exchange_support.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <stdexcept>

namespace RTBKIT {

using nlohmann::json;

namespace {

template<typename T, size_t N>
const T& pick(std::mt19937_64& rng, const std::array<T, N>& values)
{
    return values[std::uniform_int_distribution<size_t>(0, N - 1)(rng)];
}

std::string hex(std::mt19937_64& rng, int digits)
{
    static const char* chars = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < digits; ++i) out += chars[rng() & 15];
    return out;
}

std::string ipAddress(std::mt19937_64& rng)
{
    std::uniform_int_distribution<int> octet(1, 254);
    return std::to_string(octet(rng)) + "." + std::to_string(octet(rng)) + "."
        + std::to_string(octet(rng)) + "." + std::to_string(octet(rng));
}

} // namespace

json generateOpenRtbRequest(std::mt19937_64& rng, const MockRequestOptions& options)
{
    static std::atomic<uint64_t> counter{0};

    static const std::array<std::pair<int, int>, 2> sizes{{{300, 250}, {728, 90}}};
    static const std::array<const char*, 6> hosts{
        "news.example.com", "sport.example.org", "blog.example.net",
        "shop.example.com", "video.example.tv", "mail.example.io"};
    static const std::array<const char*, 5> paths{
        "/", "/article/1", "/article/2", "/category/tech", "/search?q=rtb"};
    static const std::array<const char*, 5> languages{"en", "fr", "de", "es", "ja"};
    static const std::array<const char*, 5> countries{"USA", "CAN", "FRA", "DEU", "JPN"};
    static const std::array<const char*, 3> agents{
        "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 Safari/605.1.15",
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) Gecko/20100101 Firefox/120.0",
        "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 Chrome/120.0 Safari/537.36"};

    std::string host = pick(rng, hosts);
    std::string page = "http://" + host + pick(rng, paths);

    json imps = json::array();
    int numImps = std::uniform_int_distribution<int>(0, 3)(rng) == 0 ? 2 : 1;
    for (int i = 0; i < numImps; ++i) {
        auto [w, h] = pick(rng, sizes);
        json banner = {{"w", w}, {"h", h},
                       {"pos", std::uniform_int_distribution<int>(0, 3)(rng)}};
        if (std::uniform_int_distribution<int>(0, 4)(rng) == 0)
            banner["format"] = json::array({json{{"w", 300}, {"h", 250}},
                                            json{{"w", 728}, {"h", 90}}});
        json imp = {{"id", std::to_string(i + 1)}, {"banner", banner}};
        if (std::uniform_real_distribution<double>(0, 1)(rng) >= options.noFloorProbability) {
            double floor = std::uniform_real_distribution<double>(
                0.05, options.maxFloorCpm)(rng);
            imp["bidfloor"] = std::round(floor * 100) / 100;
            imp["bidfloorcur"] = "USD";
        }
        imps.push_back(std::move(imp));
    }

    std::string id = std::to_string(counter.fetch_add(1)) + "-" + hex(rng, 12);
    const char* country = pick(rng, countries);

    return json{
        {"id", id},
        {"at", 2},
        {"tmax", options.tmaxMs},
        {"cur", json::array({"USD"})},
        {"imp", imps},
        {"site", {{"id", host}, {"domain", host}, {"page", page},
                  {"publisher", {{"id", "pub-" + host}}}}},
        {"device", {{"ip", ipAddress(rng)}, {"ua", pick(rng, agents)},
                    {"language", pick(rng, languages)},
                    {"geo", {{"country", country}}}}},
        {"user", {{"id", hex(rng, 24)}, {"buyeruid", hex(rng, 24)},
                  {"geo", {{"country", country}}}}}};
}

std::pair<std::string, std::string> splitUrl(const std::string& url)
{
    auto scheme = url.find("://");
    if (scheme == std::string::npos || scheme == 0)
        throw std::invalid_argument("bad url (no scheme): " + url);
    auto pathStart = url.find('/', scheme + 3);
    std::string origin = url.substr(0, pathStart);
    if (origin.size() == scheme + 3)
        throw std::invalid_argument("bad url (no host): " + url);
    if (pathStart == std::string::npos) return {origin, "/"};
    return {origin, url.substr(pathStart)};
}

std::string replaceAuctionPrice(const std::string& url, double priceCpm)
{
    static const std::string macro = "${AUCTION_PRICE}";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6f", priceCpm);
    std::string price = buf;
    // Trim trailing zeros but keep one digit after the dot.
    while (price.size() > 2 && price.back() == '0' && price[price.size() - 2] != '.')
        price.pop_back();

    std::string out = url;
    for (size_t pos = out.find(macro); pos != std::string::npos;
         pos = out.find(macro, pos + price.size()))
        out.replace(pos, macro.size(), price);
    return out;
}

std::string urlEncode(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out += char(c);
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string queryParam(const std::string& url, const std::string& name)
{
    auto q = url.find('?');
    if (q == std::string::npos) return {};
    size_t pos = q + 1;
    while (pos <= url.size()) {
        size_t end = url.find('&', pos);
        if (end == std::string::npos) end = url.size();
        std::string pair = url.substr(pos, end - pos);
        auto eq = pair.find('=');
        if (pair.substr(0, eq) == name && eq != std::string::npos) {
            std::string raw = pair.substr(eq + 1), out;
            for (size_t i = 0; i < raw.size(); ++i) {
                if (raw[i] == '%' && i + 2 < raw.size()
                    && std::isxdigit(static_cast<unsigned char>(raw[i + 1]))
                    && std::isxdigit(static_cast<unsigned char>(raw[i + 2]))) {
                    out += char(std::stoi(raw.substr(i + 1, 2), nullptr, 16));
                    i += 2;
                }
                else out += raw[i];
            }
            return out;
        }
        pos = end + 1;
    }
    return {};
}

double percentile(std::vector<double>& values, double p)
{
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    p = std::clamp(p, 0.0, 1.0);
    size_t index = size_t(std::ceil(p * double(values.size())));
    index = index == 0 ? 0 : index - 1;
    return values[std::min(index, values.size() - 1)];
}

} // namespace RTBKIT

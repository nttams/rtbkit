// Port of rtbkit/core/agent_configuration/include_exclude.h: the include /
// exclude lists used by agent and creative filters. Regexes are std::regex
// (ECMAScript) run over UTF-8 strings.
#pragma once

#include <algorithm>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace RTBKIT {

template<typename T>
inline bool matches(const T& t1, const T& t2)
{
    return t1 == t2;
}

struct CachedRegex {
    std::string str;
    std::regex base;

    CachedRegex() = default;
    CachedRegex(const std::string& s) : str(s), base(s) {}
    CachedRegex(const char* s) : CachedRegex(std::string(s)) {}

    bool operator<(const CachedRegex& other) const { return str < other.str; }
};

inline bool matches(const CachedRegex& rex, const std::string& val)
{
    return std::regex_search(val, rex.base);
}

/** Domain filter entry. Always a literal domain, like the original. */
struct DomainMatcher {
    DomainMatcher() = default;
    DomainMatcher(const std::string& val) : str(val) {}

    std::string str;

    bool operator<(const DomainMatcher& other) const { return str < other.str; }
};

namespace detail {

// Converts one JSON array element into a filter value.
inline void parseValue(const nlohmann::json& j, std::string& out)
{
    out = j.get<std::string>();
}

inline void parseValue(const nlohmann::json& j, int& out)
{
    out = j.get<int>();
}

inline void parseValue(const nlohmann::json& j, DomainMatcher& out)
{
    out = DomainMatcher(j.get<std::string>());
}

inline void parseValue(const nlohmann::json& j, CachedRegex& out)
{
    try {
        out = CachedRegex(j.get<std::string>());
    }
    catch (const std::regex_error& e) {
        throw std::invalid_argument("invalid regex " + j.dump() + ": " + e.what());
    }
}

} // namespace detail

template<typename T, typename IE = std::vector<T> >
struct IncludeExclude {
    IE include;
    IE exclude;

    bool empty() const { return include.empty() && exclude.empty(); }

    /** Parses {"include": [...], "exclude": [...]}. Throws
        std::invalid_argument on unknown keys or bad values; `name` is only
        used in the message. The lists come back sorted. */
    static IncludeExclude fromJson(const nlohmann::json& j, const std::string& name)
    {
        if (!j.is_object())
            throw std::invalid_argument("filter " + name + " must be an object");

        IncludeExclude result;
        for (const auto& [key, list] : j.items()) {
            if (key != "include" && key != "exclude")
                throw std::invalid_argument("filter " + name
                                            + " has invalid key: " + key);
            if (!list.is_array())
                throw std::invalid_argument("filter " + name + ": " + key
                                            + " must be an array");

            IE& out = key == "include" ? result.include : result.exclude;
            for (const auto& entry : list) {
                T value;
                try {
                    detail::parseValue(entry, value);
                }
                catch (const nlohmann::json::exception&) {
                    throw std::invalid_argument("error parsing " + entry.dump()
                                                + " in " + name);
                }
                out.push_back(std::move(value));
            }
        }

        std::sort(result.include.begin(), result.include.end());
        std::sort(result.exclude.begin(), result.exclude.end());
        return result;
    }
};

} // namespace RTBKIT

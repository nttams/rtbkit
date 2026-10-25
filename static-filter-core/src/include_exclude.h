// Port of rtbkit/core/agent_configuration/include_exclude.h (only what the
// static filters use). Regexes are std::regex (ECMAScript) over UTF-8 strings.
#pragma once

#include <regex>
#include <string>
#include <vector>

namespace RTBKIT {

template<typename T>
inline bool matches(const T& t1, const T& t2)
{
    return t1 == t2;
}

struct CachedRegex {
    std::string str;
    std::regex base;

    CachedRegex() {}
    CachedRegex(const std::string& s) : str(s), base(s) {}
};

inline bool matches(const CachedRegex& rex, const std::string& val)
{
    return std::regex_search(val, rex.base);
}

/** Domain filter entry. Always a literal domain, like the original. */
struct DomainMatcher {
    DomainMatcher() {}
    DomainMatcher(const std::string& val) : str(val) {}

    std::string str;
};

template<typename T, typename IE = std::vector<T> >
struct IncludeExclude {
    IE include;
    IE exclude;

    bool empty() const { return include.empty() && exclude.empty(); }
};

} // namespace RTBKIT

// Port of rtbkit/core/router/filters/generic_filters.h (only the helpers the
// static filters need).
#pragma once

#include "agent_config.h"
#include "filter.h"
#include "include_exclude.h"

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace RTBKIT {


/******************************************************************************/
/* FILTER BASE T                                                              */
/******************************************************************************/

/** Convenience base class for filters. */
template<typename Filter>
struct FilterBaseT : public FilterBase
{
    std::string name() const { return Filter::name; }

    void addConfig(unsigned cfgIndex, const std::shared_ptr<AgentConfig>& config)
    {
        setConfig(cfgIndex, *config, true);
    }

    void removeConfig(
            unsigned cfgIndex, const std::shared_ptr<AgentConfig>& config)
    {
        setConfig(cfgIndex, *config, false);
    }

    // If value is true then assume addConfig semantics else assume removeConfig
    // semantics. Exists for convenience.
    virtual void setConfig(unsigned, const AgentConfig&, bool)
    {
        ExcAssert(false);
    }
};


/******************************************************************************/
/* ITERATIVE FILTER                                                           */
/******************************************************************************/

/** Simplified filter base class at the cost of runtime performance. The filter
    should override and implement the filterConfig function.
 */
template<typename Filter>
struct IterativeFilter : public FilterBaseT<Filter>
{
    virtual void addConfig(
            unsigned cfgIndex,
            const std::shared_ptr<AgentConfig>& config)
    {
        if (cfgIndex >= configs.size())
            configs.resize(cfgIndex + 1);

        configs[cfgIndex] = config;
    }

    virtual void removeConfig(
            unsigned cfgIndex,
            const std::shared_ptr<AgentConfig>&)
    {
        configs[cfgIndex].reset();
    }

    virtual void filter(FilterState& state) const
    {
        ConfigSet matches = state.configs();

        for (size_t i = matches.next();
             i < matches.size();
             i = matches.next(i+1))
        {
            ExcAssert(configs[i]);

            if (filterConfig(state, *configs[i])) continue;
            matches.reset(i);
        }

        state.narrowConfigs(matches);
    }

    // Returns true if the config should be kept.
    virtual bool filterConfig(FilterState&, const AgentConfig&) const
    {
        ExcAssert(false);
        return false;
    }

protected:
    std::vector< std::shared_ptr<AgentConfig> > configs;
};


/******************************************************************************/
/* INTERVAL FILTER                                                            */
/******************************************************************************/

template<typename T>
struct IntervalFilter
{
    template<typename List>
    bool isEmpty(const List& list) const
    {
        return list.empty();
    }

    template<typename List>
    void addConfig(unsigned cfgIndex, const List& list)
    {
        for (const auto& value : list)
            addConfig(cfgIndex, value);
    }

    template<typename List>
    void removeConfig(unsigned cfgIndex, const List& list)
    {
        for (const auto& value : list)
            removeConfig(cfgIndex, value);
    }

    ConfigSet filter(T value) const
    {
        ConfigSet matches;

        for (size_t i = 0; i < intervals.size(); ++i) {
            if (value < intervals[i].lowerBound) break;
            if (!intervals[i].contains(value)) continue;

            matches |= intervals[i].configs;
        }

        return matches;
    }

private:

    struct Interval
    {
        Interval(unsigned cfgIndex, T lb, T ub) :
            lowerBound(lb), upperBound(ub)
        {
            configs.set(cfgIndex);
        }

        T lowerBound;
        T upperBound;
        ConfigSet configs;

        bool contains(T value) const
        {
            return lowerBound <= value && upperBound > value;
        }

        bool operator== (const Interval& other) const
        {
            return
                lowerBound == other.lowerBound &&
                upperBound == other.upperBound;
        }
    };

    std::vector<Interval> intervals;

    void addConfig(unsigned cfgIndex, const UserPartition::Interval& interval)
    {
        addConfig(Interval(cfgIndex, interval.first, interval.last));
    }

    void removeConfig(unsigned cfgIndex, const UserPartition::Interval& interval)
    {
        removeConfig(Interval(cfgIndex, interval.first, interval.last));
    }

    void addConfig(const Interval& interval)
    {
        std::ptrdiff_t index = findInterval(interval);

        if (index < 0)
            intervals.push_back(interval);

        else if (interval == intervals[index])
            intervals[index].configs |= interval.configs;

        else intervals.insert(intervals.begin() + index, interval);
    }

    void removeConfig(const Interval& interval)
    {
        std::ptrdiff_t index = findInterval(interval);

        if (index < 0) return;

        if (interval == intervals[index])
            intervals[index].configs &= interval.configs.negate();
    }

    std::ptrdiff_t findInterval(const Interval& interval)
    {
        for (size_t i = 0; i < intervals.size(); ++i) {
            if (interval == intervals[i]) return i;
            if (interval.lowerBound < intervals[i].lowerBound) return i;
        }
        return -1;
    }
};


/******************************************************************************/
/* DOMAIN FILTER                                                              */
/******************************************************************************/

template<typename Str>
struct DomainFilter
{
    template<typename List>
    bool isEmpty(const List& list) const
    {
        return list.empty();
    }

    template<typename List>
    void addConfig(unsigned cfgIndex, const List& list)
    {
        for (const auto& value : list)
            addConfig(cfgIndex, value);
    }

    template<typename List>
    void removeConfig(unsigned cfgIndex, const List& list)
    {
        for (const auto& value : list)
            removeConfig(cfgIndex, value);
    }

    ConfigSet filter(const Url& host) const
    {
        ConfigSet matches;

        for (const auto& key : getKeys(host)) {
            auto it = domainMap.find(key);
            if (it == domainMap.end()) continue;

            matches |= it->second;
        }

        return matches;
    }

private:

    void addConfig(unsigned cfgIndex, const DomainMatcher& matcher)
    {
        addConfig(cfgIndex, matcher.str);
    }

    void removeConfig(unsigned cfgIndex, const DomainMatcher& matcher)
    {
        removeConfig(cfgIndex, matcher.str);
    }

    void addConfig(unsigned cfgIndex, const Str& host)
    {
        domainMap[host].set(cfgIndex);
    }

    void removeConfig(unsigned cfgIndex, const Str& host)
    {
        domainMap[host].reset(cfgIndex);
    }

    // "a.b.com" gives { "a.b.com", "b.com", "com" }.
    std::vector<std::string> getKeys(const Url& host) const
    {
        std::vector<std::string> keys;

        std::string domain = host.host();
        while (true) {
            keys.push_back(domain);

            size_t pos = domain.find('.');
            if (pos == std::string::npos) break;
            domain = domain.substr(pos+1);
        }

        return keys;
    }

    std::unordered_map<std::string, ConfigSet> domainMap;
};


/******************************************************************************/
/* REGEX FILTER                                                               */
/******************************************************************************/

/** Generic include filter for regexes. */
struct RegexFilter
{
    template<typename List>
    bool isEmpty(const List& list) const
    {
        return list.empty();
    }

    template<typename List>
    void addConfig(unsigned cfgIndex, const List& list)
    {
        for (const auto& value : list)
            addConfig(cfgIndex, value);
    }

    template<typename List>
    void removeConfig(unsigned cfgIndex, const List& list)
    {
        for (const auto& value : list)
            removeConfig(cfgIndex, value);
    }

    ConfigSet filter(const std::string& str) const
    {
        ConfigSet matches;

        for (const auto& entry : data)
            matches |= entry.second.filter(str);

        return matches;
    }

private:

    void addConfig(unsigned cfgIndex, const CachedRegex& regex)
    {
        auto res = data.try_emplace(regex.str);
        if (res.second) res.first->second.regex = regex;
        res.first->second.configs.set(cfgIndex);
    }

    void removeConfig(unsigned cfgIndex, const CachedRegex& regex)
    {
        auto it = data.find(regex.str);
        if (it == data.end()) return;

        it->second.configs.reset(cfgIndex);
        if (it->second.configs.empty()) data.erase(it);
    }

    struct RegexData
    {
        CachedRegex regex;
        ConfigSet configs;

        ConfigSet filter(const std::string& str) const
        {
            return RTBKIT::matches(regex, str) ? configs : ConfigSet();
        }
    };

    std::map<std::string, RegexData> data;
};


/******************************************************************************/
/* LIST FILTER                                                                */
/******************************************************************************/

template<typename T, typename List = std::vector<T> >
struct ListFilter
{
    bool isEmpty(const List& list) const
    {
        return list.empty();
    }

    void addConfig(unsigned cfgIndex, const List& list)
    {
        setConfig(cfgIndex, list, true);
    }

    void removeConfig(unsigned cfgIndex, const List& list)
    {
        setConfig(cfgIndex, list, false);
    }

    ConfigSet filter(const T& value) const
    {
        auto it = data.find(value);
        return it == data.end() ? ConfigSet() : it->second;
    }

    ConfigSet filter(const List& list) const
    {
        ConfigSet configs;

        for (const auto& entry : list) {

            auto it = data.find(entry);
            if (it == data.end()) continue;

            configs |= it->second;
        }

        return configs;
    }

private:

    void setConfig(unsigned cfgIndex, const List& list, bool value)
    {
        for (const auto& entry : list)
            data[entry].set(cfgIndex, value);
    }

    std::unordered_map<T, ConfigSet> data;
};


/******************************************************************************/
/* SEGMENT LIST FILTER                                                        */
/******************************************************************************/

/** Segments have quirks and are best handled separately from the list filter.
 */
struct SegmentListFilter
{
    bool isEmpty(const SegmentList& segments) const
    {
        return segments.empty();
    }

    void addConfig(unsigned cfgIndex, const SegmentList& segments)
    {
        setConfig(cfgIndex, segments, true);
    }

    void removeConfig(unsigned cfgIndex, const SegmentList& segments)
    {
        setConfig(cfgIndex, segments, false);
    }

    ConfigSet filter(int i, const std::string& str) const
    {
        return i >= 0 ? get(intSet, i) : get(strSet, str);
    }

    ConfigSet filter(const SegmentList& segments) const
    {
        ConfigSet configs;

        segments.forEach([&](int i, std::string str) {
                    configs |= filter(i, str);
                });

        return configs;
    }

private:

    void setConfig(unsigned cfgIndex, const SegmentList& segments, bool value)
    {
        segments.forEach([&](int i, std::string str) {
                    if (i >= 0) intSet[i].set(cfgIndex, value);
                    else strSet[str].set(cfgIndex, value);
                });
    }

    template<typename K>
    ConfigSet get(const std::unordered_map<K, ConfigSet>& m, const K& k) const
    {
        auto it = m.find(k);
        return it != m.end() ? it->second : ConfigSet();
    }

    std::unordered_map<int, ConfigSet> intSet;
    std::unordered_map<std::string, ConfigSet> strSet;
};


/******************************************************************************/
/* INCLUDE EXCLUDE FILTER                                                     */
/******************************************************************************/

/** Takes any filter which follows the addConfig, removeConfig, setConfig,
    filterConfig interface and turns it into a standard include/exclude filter.
 */
template<typename Filter>
struct IncludeExcludeFilter
{
    IncludeExcludeFilter() : emptyIncludes(true) {}

    template<typename... Args>
    void addInclude(unsigned cfgIndex, Args&&... args)
    {
        if (includes.isEmpty(std::forward<Args>(args)...)) return;

        includes.addConfig(cfgIndex, std::forward<Args>(args)...);
        emptyIncludes.reset(cfgIndex);
    }

    template<typename... Args>
    void addExclude(unsigned cfgIndex, Args&&... args)
    {
        if (excludes.isEmpty(std::forward<Args>(args)...)) return;

        excludes.addConfig(cfgIndex, std::forward<Args>(args)...);
    }

    template<typename T, typename IE = std::vector<T> >
    void addIncludeExclude(unsigned cfgIndex, const IncludeExclude<T, IE>& ie)
    {
        addInclude(cfgIndex, ie.include);
        addExclude(cfgIndex, ie.exclude);
    }


    template<typename... Args>
    void removeInclude(unsigned cfgIndex, Args&&... args)
    {
        if (includes.isEmpty(std::forward<Args>(args)...)) return;

        includes.removeConfig(cfgIndex, std::forward<Args>(args)...);
        emptyIncludes.set(cfgIndex);
    }

    template<typename... Args>
    void removeExclude(unsigned cfgIndex, Args&&... args)
    {
        if (excludes.isEmpty(std::forward<Args>(args)...)) return;

        excludes.removeConfig(cfgIndex, std::forward<Args>(args)...);
    }

    template<typename T, typename IE = std::vector<T> >
    void removeIncludeExclude(unsigned cfgIndex, const IncludeExclude<T, IE>& ie)
    {
        removeInclude(cfgIndex, ie.include);
        removeExclude(cfgIndex, ie.exclude);
    }


    template<typename... Args>
    void setInclude(unsigned cfgIndex, bool value, Args&&... args)
    {
        if (value) addInclude(cfgIndex, std::forward<Args>(args)...);
        else removeInclude(cfgIndex, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void setExclude(unsigned cfgIndex, bool value, Args&&... args)
    {
        if (value) addExclude(cfgIndex, std::forward<Args>(args)...);
        else removeExclude(cfgIndex, std::forward<Args>(args)...);
    }

    template<typename T, typename IE = std::vector<T> >
    void setIncludeExclude(
            unsigned cfgIndex, bool value, const IncludeExclude<T, IE>& ie)
    {
        if (value) addIncludeExclude(cfgIndex, ie);
        else removeIncludeExclude(cfgIndex, ie);
    }


    template<typename... Args>
    ConfigSet filter(Args&&... args) const
    {
        ConfigSet configs = emptyIncludes;

        configs |= includes.filter(std::forward<Args>(args)...);
        if (configs.empty()) return configs;

        configs &= excludes.filter(std::forward<Args>(args)...).negate();
        return configs;
    }


private:
    ConfigSet emptyIncludes;
    Filter includes;
    Filter excludes;
};


} // namespace RTBKIT

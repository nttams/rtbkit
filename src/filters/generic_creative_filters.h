// Port of rtbkit/core/router/filters/generic_creative_filters.h: the building
// blocks for filters that work per creative instead of per config.
#pragma once

#include "filters/generic_filters.h"

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace RTBKIT {


/******************************************************************************/
/* CREATIVE FILTER                                                            */
/******************************************************************************/

/** Convenience base class for creative filters. */
template<typename Filter>
struct CreativeFilter : public FilterBaseT<Filter>
{
    // Same semantics as addConfig but called for every creatives.
    virtual void addCreative(
            unsigned cfgIndex, unsigned crIndex, const Creative& creative) = 0;

    void addConfig(unsigned cfgIndex,
                   const std::shared_ptr<AgentConfig>& config) override
    {
        for (size_t i = 0; i < config->creatives.size(); ++i)
            addCreative(cfgIndex, unsigned(i), config->creatives[i]);
    }

    // Same semantics as removeConfig but called for every creatives.
    virtual void removeCreative(
            unsigned cfgIndex, unsigned crIndex, const Creative& creative) = 0;

    void removeConfig(unsigned cfgIndex,
                      const std::shared_ptr<AgentConfig>& config) override
    {
        for (size_t i = 0; i < config->creatives.size(); ++i)
            removeCreative(cfgIndex, unsigned(i), config->creatives[i]);
    }

    // Same semantics as filter but called for every impressions.
    virtual void filterImpression(
            FilterState&, unsigned /*impId*/, const AdSpot&) const
    {
        RTB_ASSERT(false);
    }

    void filter(FilterState& state) const override
    {
        for (size_t i = 0; i < state.request.imp.size(); ++i) {
            filterImpression(state, unsigned(i), state.request.imp[i]);
            if (state.configs().empty()) break;
        }
    }
};


/******************************************************************************/
/* ITERATIVE CREATIVE FILTER                                                  */
/******************************************************************************/

/** Simplified filter base class at the cost of runtime performance. The filter
    should override and implement the filterCreative or filterImpression
    functions. */
template<typename Filter>
struct IterativeCreativeFilter : public IterativeFilter<Filter>
{
    void filter(FilterState& state) const override
    {
        for (size_t impId = 0; impId < state.request.imp.size(); ++impId) {
            const auto& imp = state.request.imp[impId];
            auto mask = filterImpression(state, impId, imp);
            state.narrowCreativesForImp(unsigned(impId), mask);
        }
    }

    // Same semantics as filter but called for every impressions.
    virtual CreativeMatrix
    filterImpression(FilterState& state, size_t impId, const AdSpot& imp) const
    {
        CreativeMatrix mask;
        auto active = state.creatives(unsigned(impId));

        ConfigSet matches = state.configs();
        for (size_t cfgId = matches.next();
             cfgId < matches.size();
             cfgId = matches.next(cfgId + 1))
        {
            const auto& cfg = *this->configs[cfgId];

            for (size_t crId = 0; crId < cfg.creatives.size(); ++crId) {
                if (!active[crId][cfgId]) continue;

                if (filterCreative(state, imp, cfg, cfg.creatives[crId]))
                    mask.set(crId, cfgId);
            }
        }

        return mask;
    }

    // Same semantics as filter but called for every impression, agent config,
    // creatives combinations.
    virtual bool
    filterCreative(FilterState&, const AdSpot&, const AgentConfig&,
                   const Creative&) const
    {
        RTB_ASSERT(false);
        return false;
    }
};


/******************************************************************************/
/* CREATIVE REGEX FILTER                                                      */
/******************************************************************************/

/** Generic include filter for regexes. */
struct CreativeRegexFilter
{
    template<typename List>
    bool isEmpty(const List& list) const
    {
        return list.empty();
    }

    template<typename List>
    void addConfig(unsigned cfgIndex, unsigned creativeId, const List& list)
    {
        for (const auto& value : list)
            addConfig(cfgIndex, creativeId, value);
    }

    template<typename List>
    void removeConfig(unsigned cfgIndex, unsigned creativeId, const List& list)
    {
        for (const auto& value : list)
            removeConfig(cfgIndex, creativeId, value);
    }

    CreativeMatrix filter(const std::string& str) const
    {
        CreativeMatrix matches;

        for (const auto& entry : data)
            matches |= entry.second.filter(str);

        return matches;
    }

private:

    void addConfig(unsigned cfgIndex, unsigned creativeId, const CachedRegex& regex)
    {
        auto res = data.try_emplace(regex.str);
        if (res.second) res.first->second.regex = regex;
        res.first->second.creatives.set(creativeId, cfgIndex);
    }

    void removeConfig(unsigned cfgIndex, unsigned creativeId, const CachedRegex& regex)
    {
        auto it = data.find(regex.str);
        if (it == data.end()) return;

        it->second.creatives.reset(creativeId, cfgIndex);
        if (it->second.creatives.empty()) data.erase(it);
    }

    struct RegexData
    {
        CachedRegex regex;
        CreativeMatrix creatives;

        CreativeMatrix filter(const std::string& str) const
        {
            return RTBKIT::matches(regex, str) ? creatives : CreativeMatrix();
        }
    };

    std::map<std::string, RegexData> data;
};


/******************************************************************************/
/* CREATIVE LIST FILTER                                                       */
/******************************************************************************/

template<typename T, typename List = std::vector<T> >
struct CreativeListFilter
{
    bool isEmpty(const List& list) const
    {
        return list.empty();
    }

    void addConfig(unsigned cfgIndex, unsigned creativeId, const List& list)
    {
        setConfig(cfgIndex, creativeId, list, true);
    }

    void removeConfig(unsigned cfgIndex, unsigned creativeId, const List& list)
    {
        setConfig(cfgIndex, creativeId, list, false);
    }

    CreativeMatrix filter(const T& value) const
    {
        auto it = data.find(value);
        return it == data.end() ? CreativeMatrix() : it->second;
    }

    CreativeMatrix filter(const List& list) const
    {
        CreativeMatrix configs;

        for (const auto& entry : list) {
            auto it = data.find(entry);
            if (it == data.end()) continue;

            configs |= it->second;
        }

        return configs;
    }

private:

    void setConfig(unsigned cfgIndex, unsigned creativeId,
                   const List& list, bool value)
    {
        for (const auto& entry : list)
            data[entry].set(creativeId, cfgIndex, value);
    }

    std::unordered_map<T, CreativeMatrix> data;
};


/******************************************************************************/
/* CREATIVE INCLUDE EXCLUDE FILTER                                            */
/******************************************************************************/

template<typename Filter>
struct CreativeIncludeExcludeFilter
{
    CreativeIncludeExcludeFilter() : emptyIncludes(true) {}

    template<typename... Args>
    void addInclude(unsigned cfgIndex, unsigned crIndex, Args&&... args)
    {
        if (includes.isEmpty(std::forward<Args>(args)...)) return;

        includes.addConfig(cfgIndex, crIndex, std::forward<Args>(args)...);
        emptyIncludes.reset(crIndex, cfgIndex);
    }

    template<typename... Args>
    void addExclude(unsigned cfgIndex, unsigned crIndex, Args&&... args)
    {
        if (excludes.isEmpty(std::forward<Args>(args)...)) return;

        excludes.addConfig(cfgIndex, crIndex, std::forward<Args>(args)...);
    }

    template<typename T, typename IE = std::vector<T> >
    void addIncludeExclude(
            unsigned cfgIndex, unsigned crIndex, const IncludeExclude<T, IE>& ie)
    {
        addInclude(cfgIndex, crIndex, ie.include);
        addExclude(cfgIndex, crIndex, ie.exclude);
    }

    template<typename... Args>
    void removeInclude(unsigned cfgIndex, unsigned crIndex, Args&&... args)
    {
        if (includes.isEmpty(std::forward<Args>(args)...)) return;

        includes.removeConfig(cfgIndex, crIndex, std::forward<Args>(args)...);
        emptyIncludes.set(crIndex, cfgIndex);
    }

    template<typename... Args>
    void removeExclude(unsigned cfgIndex, unsigned crIndex, Args&&... args)
    {
        if (excludes.isEmpty(std::forward<Args>(args)...)) return;

        excludes.removeConfig(cfgIndex, crIndex, std::forward<Args>(args)...);
    }

    template<typename T, typename IE = std::vector<T> >
    void removeIncludeExclude(
            unsigned cfgIndex, unsigned crIndex, const IncludeExclude<T, IE>& ie)
    {
        removeInclude(cfgIndex, crIndex, ie.include);
        removeExclude(cfgIndex, crIndex, ie.exclude);
    }

    template<typename... Args>
    void setInclude(
            unsigned cfgIndex, unsigned crIndex, bool value, Args&&... args)
    {
        if (value) addInclude(cfgIndex, crIndex, std::forward<Args>(args)...);
        else removeInclude(cfgIndex, crIndex, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void setExclude(
            unsigned cfgIndex, unsigned crIndex, bool value, Args&&... args)
    {
        if (value) addExclude(cfgIndex, crIndex, std::forward<Args>(args)...);
        else removeExclude(cfgIndex, crIndex, std::forward<Args>(args)...);
    }

    template<typename T, typename IE = std::vector<T> >
    void setIncludeExclude(
            unsigned cfgIndex, unsigned crIndex, bool value,
            const IncludeExclude<T, IE>& ie)
    {
        if (value) addIncludeExclude(cfgIndex, crIndex, ie);
        else removeIncludeExclude(cfgIndex, crIndex, ie);
    }

    template<typename... Args>
    CreativeMatrix filter(Args&&... args) const
    {
        CreativeMatrix creatives = emptyIncludes;

        creatives |= includes.filter(std::forward<Args>(args)...);
        if (creatives.empty()) return creatives;

        creatives &= excludes.filter(std::forward<Args>(args)...).negate();
        return creatives;
    }

private:
    CreativeMatrix emptyIncludes;
    Filter includes;
    Filter excludes;
};


/******************************************************************************/
/* CREATIVE SEGMENT LIST FILTER                                               */
/******************************************************************************/

/** Segments have quirks and are best handled separately from the list filter. */
struct CreativeSegmentListFilter
{
    void addConfig(unsigned cfgIndex, unsigned creativeId,
                   const SegmentList& segments)
    {
        setConfig(cfgIndex, creativeId, segments, true);
    }

    void removeConfig(unsigned cfgIndex, unsigned creativeId,
                      const SegmentList& segments)
    {
        setConfig(cfgIndex, creativeId, segments, false);
    }

    bool isEmpty(const SegmentList& segments) const
    {
        return segments.empty();
    }

    CreativeMatrix filter(int i, const std::string& str) const
    {
        return i >= 0 ? get(intSet, i) : get(strSet, str);
    }

    CreativeMatrix filter(const SegmentList& segments) const
    {
        CreativeMatrix configs;

        segments.forEach([&](int i, const std::string& str) {
                    configs |= filter(i, str);
                });

        return configs;
    }

private:

    void setConfig(unsigned cfgIndex, unsigned creativeId,
                   const SegmentList& segments, bool value)
    {
        segments.forEach([&](int i, const std::string& str) {
                    if (i >= 0) intSet[i].set(creativeId, cfgIndex, value);
                    else strSet[str].set(creativeId, cfgIndex, value);
                });
    }

    template<typename K>
    CreativeMatrix get(const std::unordered_map<K, CreativeMatrix>& m,
                       const K& k) const
    {
        auto it = m.find(k);
        return it != m.end() ? it->second : CreativeMatrix();
    }

    std::unordered_map<int, CreativeMatrix> intSet;
    std::unordered_map<std::string, CreativeMatrix> strSet;
};

} // namespace RTBKIT

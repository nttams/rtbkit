// Port of rtbkit/core/router/filters/creative_filters.h: filters that decide
// which creatives of a config can be used for each ad spot.
#pragma once

#include "filters/generic_creative_filters.h"
#include "filters/priority.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace RTBKIT {


/******************************************************************************/
/* CREATIVE FORMAT FILTER                                                     */
/******************************************************************************/

struct CreativeFormatFilter : public CreativeFilter<CreativeFormatFilter>
{
    static constexpr const char* name = "CreativeFormat";
    unsigned priority() const override { return Priority::CreativeFormat; }

    void addCreative(
            unsigned cfgIndex, unsigned crIndex, const Creative& creative) override
    {
        formatFilter[makeKey(creative.format)].set(crIndex, cfgIndex);
    }

    void removeCreative(
            unsigned cfgIndex, unsigned crIndex, const Creative& creative) override
    {
        formatFilter[makeKey(creative.format)].reset(crIndex, cfgIndex);
    }

    void filterImpression(
            FilterState& state, unsigned impIndex, const AdSpot& imp) const override
    {
        // A spot without formats accepts every creative.
        if (imp.formats.empty()) return;

        // The 0x0 format means: match anything.
        CreativeMatrix creatives = get(Format(0, 0));

        for (const auto& format : imp.formats)
            creatives |= get(format);

        state.narrowCreativesForImp(impIndex, creatives);
    }

private:
    static uint32_t makeKey(const Format& format)
    {
        return uint32_t(uint16_t(format.width)) << 16 | uint16_t(format.height);
    }

    CreativeMatrix get(const Format& format) const
    {
        auto it = formatFilter.find(makeKey(format));
        return it == formatFilter.end() ? CreativeMatrix() : it->second;
    }

    std::unordered_map<uint32_t, CreativeMatrix> formatFilter;
};


/******************************************************************************/
/* CREATIVE LANGUAGE FILTER                                                   */
/******************************************************************************/

struct CreativeLanguageFilter : public CreativeFilter<CreativeLanguageFilter>
{
    static constexpr const char* name = "CreativeLanguage";
    unsigned priority() const override { return Priority::CreativeLanguage; }

    void addCreative(
            unsigned cfgIndex, unsigned crIndex, const Creative& creative) override
    {
        impl.addIncludeExclude(cfgIndex, crIndex, creative.languageFilter);
    }

    void removeCreative(
            unsigned cfgIndex, unsigned crIndex, const Creative& creative) override
    {
        impl.removeIncludeExclude(cfgIndex, crIndex, creative.languageFilter);
    }

    void filter(FilterState& state) const override
    {
        state.narrowAllCreatives(impl.filter(state.request.language));
    }

private:
    CreativeIncludeExcludeFilter< CreativeListFilter<std::string> > impl;
};


/******************************************************************************/
/* CREATIVE LOCATION FILTER                                                   */
/******************************************************************************/

struct CreativeLocationFilter : public CreativeFilter<CreativeLocationFilter>
{
    static constexpr const char* name = "CreativeLocation";
    unsigned priority() const override { return Priority::CreativeLocation; }

    void addCreative(
            unsigned cfgIndex, unsigned crIndex, const Creative& creative) override
    {
        impl.addIncludeExclude(cfgIndex, crIndex, creative.locationFilter);
    }

    void removeCreative(
            unsigned cfgIndex, unsigned crIndex, const Creative& creative) override
    {
        impl.removeIncludeExclude(cfgIndex, crIndex, creative.locationFilter);
    }

    void filter(FilterState& state) const override
    {
        state.narrowAllCreatives(
                impl.filter(state.request.location.fullLocationString()));
    }

private:
    CreativeIncludeExcludeFilter<CreativeRegexFilter> impl;
};


/******************************************************************************/
/* CREATIVE EXCHANGE NAME FILTER                                              */
/******************************************************************************/

struct CreativeExchangeNameFilter :
        public CreativeFilter<CreativeExchangeNameFilter>
{
    static constexpr const char* name = "CreativeExchangeName";
    unsigned priority() const override { return Priority::CreativeExchangeName; }

    void addCreative(
            unsigned cfgIndex, unsigned crIndex, const Creative& creative) override
    {
        impl.addIncludeExclude(cfgIndex, crIndex, creative.exchangeFilter);
    }

    void removeCreative(
            unsigned cfgIndex, unsigned crIndex, const Creative& creative) override
    {
        impl.removeIncludeExclude(cfgIndex, crIndex, creative.exchangeFilter);
    }

    void filter(FilterState& state) const override
    {
        state.narrowAllCreatives(impl.filter(state.request.exchange));
    }

private:
    CreativeIncludeExcludeFilter< CreativeListFilter<std::string> > impl;
};


/******************************************************************************/
/* CREATIVE EXCHANGE FILTER                                                   */
/******************************************************************************/

/** Asks the exchange connector about every creative. Slow, so it runs late. */
struct CreativeExchangeFilter : public IterativeFilter<CreativeExchangeFilter>
{
    static constexpr const char* name = "CreativeExchange";
    unsigned priority() const override { return Priority::CreativeExchange; }

    void filter(FilterState& state) const override;
};


/******************************************************************************/
/* CREATIVE SEGMENTS FILTER                                                   */
/******************************************************************************/

struct CreativeSegmentsFilter : public CreativeFilter<CreativeSegmentsFilter>
{
    static constexpr const char* name = "CreativeSegments";
    unsigned priority() const override { return Priority::CreativeSegments; }

    void addCreative(unsigned cfgIndex, unsigned crIndex,
                     const Creative& creative) override
    {
        setCreative(cfgIndex, crIndex, creative, true);
    }

    void removeCreative(unsigned cfgIndex, unsigned crIndex,
                        const Creative& creative) override
    {
        setCreative(cfgIndex, crIndex, creative, false);
    }

    void filter(FilterState& state) const override;

private:
    void setCreative(unsigned configIndex, unsigned crIndex,
                     const Creative& creative, bool value);

    struct SegmentData
    {
        CreativeIncludeExcludeFilter<CreativeSegmentListFilter> ie;
        CreativeMatrix excludeIfNotPresent;
    };

    std::unordered_map<std::string, SegmentData> data;
    std::unordered_set<std::string> excludeIfNotPresent;
};


/******************************************************************************/
/* CREATIVE PMP FILTER                                                        */
/******************************************************************************/

/** Private marketplace: a creative with a deal id can only be used on spots
    that offer the deal, and creatives without one only on spots that don't
    have a private auction. */
struct CreativePMPFilter : public CreativeFilter<CreativePMPFilter>
{
    static constexpr const char* name = "CreativePMP";
    unsigned priority() const override { return Priority::CreativePMP; }

    void addCreative(
            unsigned cfgIndex, unsigned crIndex, const Creative& creative) override
    {
        dealFilter[Id(creative.dealId)].set(crIndex, cfgIndex);
    }

    void removeCreative(
            unsigned cfgIndex, unsigned crIndex, const Creative& creative) override
    {
        dealFilter[Id(creative.dealId)].reset(crIndex, cfgIndex);
    }

    void filterImpression(
            FilterState& state, unsigned impIndex, const AdSpot& imp) const override
    {
        CreativeMatrix creatives;
        if (imp.pmp && imp.pmp->privateAuction == 1) {
            for (const auto& deal : imp.pmp->deals)
                creatives |= get(deal.id);
        }
        else {
            // Not a private auction: only the creatives without a deal.
            creatives |= get(Id());
        }

        state.narrowCreativesForImp(impIndex, creatives);
    }

private:
    std::unordered_map<Id, CreativeMatrix> dealFilter;

    CreativeMatrix get(const Id& dealId) const
    {
        auto it = dealFilter.find(dealId);
        return it == dealFilter.end() ? CreativeMatrix() : it->second;
    }
};


/** Creates a creative filter by name (the filters' static `name`). Returns
    null for an unknown name. */
std::unique_ptr<FilterBase> createCreativeFilter(const std::string& name);

/** Names of all creative filters. */
const std::vector<std::string>& creativeFilterNames();

} // namespace RTBKIT

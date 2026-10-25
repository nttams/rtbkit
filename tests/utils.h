// Port of rtbkit/core/router/filters/testing/utils.h on top of Catch2.
#pragma once

#include "agent/agent_config.h"
#include "common/bid_request.h"
#include "common/exchange_connector.h"
#include "common/include_exclude.h"
#include "common/segments.h"
#include "filters/filter.h"

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace RTBKIT {

namespace Test {

/******************************************************************************/
/* FILTER EXCHANGE CONNECTOR                                                  */
/******************************************************************************/

struct FilterExchangeConnector : public ExchangeConnector
{
    FilterExchangeConnector(const std::string& name) : name(name) {}

    std::string exchangeName() const override { return name; }

private:
    std::string name;
};

/******************************************************************************/
/* TITLE                                                                      */
/******************************************************************************/

inline std::string& currentTitle()
{
    static std::string title;
    return title;
}

/** Names the step being checked; it is shown when a check fails. */
inline void title(const std::string& title)
{
    currentTitle() = title;
}

/******************************************************************************/
/* CHECK                                                                      */
/******************************************************************************/

inline void check(
        const ConfigSet& configs,
        const std::initializer_list<size_t>& expected)
{
    ConfigSet ex;
    for (size_t cfg : expected) ex.set(cfg);

    ConfigSet diff = configs;
    diff ^= ex;

    if (diff.empty()) {
        SUCCEED();
        return;
    }

    FAIL_CHECK("[" << currentTitle() << "] val=" << configs.print()
               << " exp=" << ex.print() << " dif=" << diff.print());
}

inline void check(
        const CreativeMatrix& creatives,
        const std::vector< std::vector<size_t> >& expected)
{
    CreativeMatrix exp;
    for (size_t cr = 0; cr < expected.size(); ++cr)
        for (size_t cfg : expected[cr])
            exp.set(cr, cfg);

    CreativeMatrix diff = creatives;
    diff ^= exp;

    if (diff.empty()) {
        SUCCEED();
        return;
    }

    FAIL_CHECK("[" << currentTitle() << "] val=" << creatives.print()
               << " exp=" << exp.print() << " dif=" << diff.print());
}

// Runs the filter on request and checks the result against expected for
// impression imp. Expected is creative major, list of config ids
// (eg. expected[creativeId][configId]).
inline void check(
        const FilterBase& filter,
        const BidRequest& request,
        const CreativeMatrix& creatives,
        unsigned imp,
        const std::vector< std::vector<size_t> >& expected)
{
    FilterExchangeConnector conn("bob");

    FilterState state(request, &conn, creatives);
    filter.filter(state);

    check(state.creatives(imp), expected);
}

// Check function used for static filters
inline void check(
        const FilterBase& filter,
        BidRequest& request,
        const std::string exchangeName,
        const ConfigSet& mask,
        const std::initializer_list<size_t>& exp)
{
    FilterExchangeConnector conn(exchangeName);

    /* Note that some filters depends on the bid request's exchange field while
       others depend on the exchange connector's name. Now you might think that
       they're always the same but you'd be wrong. To alleviate my endless pain
       on this subject, let's just fudge it here and call it a day.
     */
    request.exchange = exchangeName;

    // A bid request without ad spots doesn't really make any sense and will
    // accidently make state.configs() to return an empty set.
    request.imp.emplace_back();

    CreativeMatrix activeConfigs;
    for (size_t i = mask.next(); i < mask.size(); i = mask.next(i+1))
        activeConfigs.setConfig(i, 1);

    FilterState state(request, &conn, activeConfigs);

    filter.filter(state);
    check(state.configs() & mask, exp);
}


/******************************************************************************/
/* IE                                                                         */
/******************************************************************************/

template<typename T, typename List = std::vector<T> >
IncludeExclude<T, List>
ie(     const std::initializer_list<T>& includes,
        const std::initializer_list<T>& excludes)
{
    IncludeExclude<T, List> ie;
    for (const auto& v : includes) ie.include.push_back(v);
    for (const auto& v : excludes) ie.exclude.push_back(v);
    return ie;
}

template<typename T, typename List = std::vector<T> >
IncludeExclude<T, List> ie()
{
    return IncludeExclude<T, List>();
}

/******************************************************************************/
/* SEGMENT                                                                    */
/******************************************************************************/

inline void segmentImpl(SegmentList&) {}

template<typename Arg, typename... Args>
void segmentImpl(SegmentList& seg, Arg&& arg, Args&&... rest)
{
    seg.add(std::forward<Arg>(arg));
    segmentImpl(seg, std::forward<Args>(rest)...);
}

template<typename... Args>
SegmentList segment(Args&&... args)
{
    SegmentList seg;
    segmentImpl(seg, std::forward<Args>(args)...);
    seg.sort();
    return seg;
}

inline void addSegment(
        BidRequest& br,
        const std::string& name,
        const SegmentList& segment)
{
    br.segments[name] = std::make_shared<SegmentList>(segment);
}

inline void addSegment(
        AgentConfig& config,
        const std::string& name,
        bool excludeIfNotPresent,
        const SegmentList& includes,
        const SegmentList& excludes,
        const IncludeExclude<std::string>& exchangeIE)
{
    AgentConfig::SegmentInfo seg;

    seg.excludeIfNotPresent = excludeIfNotPresent;
    seg.include = includes;
    seg.exclude = excludes;
    seg.applyToExchanges = exchangeIE;

    config.segments[name] = seg;
}

/******************************************************************************/
/* IMPRESSION                                                                 */
/******************************************************************************/

inline void addImp(
        BidRequest& request,
        AdPosition pos,
        const std::initializer_list<Format>& formats)
{
    AdSpot imp;
    for (const auto& format : formats) imp.formats.push_back(format);
    imp.position = pos;
    request.imp.push_back(imp);
}

/******************************************************************************/
/* ADD/REMOVE CONFIG                                                          */
/******************************************************************************/

inline std::shared_ptr<AgentConfig> unowned(AgentConfig& cfg)
{
    return std::shared_ptr<AgentConfig>(&cfg, [] (AgentConfig*) {});
}

inline void addConfig(FilterBase& filter, unsigned cfgIndex, AgentConfig& cfg)
{
    filter.addConfig(cfgIndex, unowned(cfg));
}

inline void addConfig(
        FilterBase& filter,
        unsigned cfgIndex, AgentConfig& cfg,
        CreativeMatrix& creatives)
{
    addConfig(filter, cfgIndex, cfg);
    creatives.setConfig(cfgIndex, cfg.creatives.size());
}

inline void removeConfig(FilterBase& filter, unsigned cfgIndex, AgentConfig& cfg)
{
    filter.removeConfig(cfgIndex, unowned(cfg));
}

inline void removeConfig(
        FilterBase& filter,
        unsigned cfgIndex, AgentConfig& cfg,
        CreativeMatrix& creatives)
{
    removeConfig(filter, cfgIndex, cfg);
    creatives.resetConfig(cfgIndex);
}

} // namespace Test

} // namespace RTBKIT

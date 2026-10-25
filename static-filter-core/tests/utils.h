// Port of rtbkit/core/router/filters/testing/utils.h without boost.test.
#pragma once

#include "agent_config.h"
#include "bid_request.h"
#include "exchange_connector.h"
#include "filter.h"
#include "include_exclude.h"
#include "segments.h"

#include <initializer_list>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace RTBKIT {

namespace Test {

inline int& failures() { static int n = 0; return n; }

#define TEST_CHECK(_expr_)                                                  \
    do {                                                                    \
        if (!(_expr_)) {                                                    \
            std::cerr << __FILE__ << ":" << __LINE__                        \
                      << ": check failed: " #_expr_ << std::endl;           \
            ++RTBKIT::Test::failures();                                     \
        }                                                                   \
    } while (0)

/******************************************************************************/
/* FILTER EXCHANGE CONNECTOR                                                  */
/******************************************************************************/

struct FilterExchangeConnector : public ExchangeConnector
{
    FilterExchangeConnector(const std::string& name) : name(name) {}

    std::string exchangeName() const { return name; }

private:
    std::string name;
};

/******************************************************************************/
/* TITLE                                                                      */
/******************************************************************************/

inline void title(const std::string& title)
{
    std::string padding(80 - 4 - title.size(), '-');
    std::cerr << "[ " << title << " ]" << padding << std::endl;
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

    if (diff.empty()) return;

    std::cerr << "val=" << configs.print() << std::endl
        << "exp=" << ex.print() << std::endl
        << "dif=" << diff.print() << std::endl;
    TEST_CHECK(false);
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

    // A bid request without ad spots doesn't really make any sense.
    request.imp.emplace_back();

    ConfigSet activeConfigs;
    for (size_t i = mask.next(); i < mask.size(); i = mask.next(i+1))
        activeConfigs.set(i);

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

inline void removeConfig(FilterBase& filter, unsigned cfgIndex, AgentConfig& cfg)
{
    filter.removeConfig(cfgIndex, unowned(cfg));
}

} // namespace Test

} // namespace RTBKIT

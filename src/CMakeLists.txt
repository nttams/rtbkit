rtb_add_library(rtb_common
    SOURCES
        common/account_key.cc
        common/bid_request.cc
        common/currency.cc
        common/openrtb_parser.cc
        common/segments.cc
    DEPS nlohmann_json::nlohmann_json)

rtb_add_library(rtb_agent_config
    SOURCES
        agent/agent_config.cc
    DEPS rtb_common)

rtb_add_library(rtb_filters
    SOURCES
        filters/creative_filters.cc
        filters/filter.cc
        filters/filter_pool.cc
        filters/static_filters.cc
    DEPS rtb_agent_config rtb_common)

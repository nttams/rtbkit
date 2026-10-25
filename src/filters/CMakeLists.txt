rtb_add_library(rtb_filters
    SOURCES
        creative_filters.cc
        filter.cc
        filter_pool.cc
        static_filters.cc
    DEPS rtb_agent_config rtb_common)

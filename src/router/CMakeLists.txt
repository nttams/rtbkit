# Owner: router+agents. Tests go in tests/router/*_test.cc (rtb_router_tests).
rtb_add_library(rtb_router
    SOURCES
        router.cc
    DEPS rtb_filters rtb_agent_protocol rtb_agent_config rtb_banker
         rtb_exchange rtb_common cppzmq Threads::Threads)

rtb_add_area_tests(router DEPS rtb_router)

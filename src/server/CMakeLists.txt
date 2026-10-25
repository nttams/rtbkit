# The core server: banker + router + OpenRTB connector + post auction + ad
# server (+ admin HTTP) wired together. Used by apps/rtbkit_router,
# apps/rtbkit_demo and the integration test.
rtb_add_library(rtb_server
    SOURCES
        core_server.cc
    DEPS rtb_router rtb_post_auction rtb_adserver rtb_exchange rtb_banker
         rtb_common httplib::httplib Threads::Threads)

# End to end tests: tests/integration/*_test.cc (rtb_integration_tests).
rtb_add_area_tests(integration DEPS rtb_server rtb_bidding_agent)

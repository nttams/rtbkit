# Owner: post-auction. Tests go in tests/adserver/*_test.cc (rtb_adserver_tests).
rtb_add_library(rtb_adserver
    SOURCES
        adserver.cc
    DEPS rtb_post_auction rtb_common httplib::httplib Threads::Threads)

rtb_add_area_tests(adserver DEPS rtb_adserver)

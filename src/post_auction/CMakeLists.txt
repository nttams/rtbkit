# Owner: post-auction. Tests go in tests/post_auction/*_test.cc
# (rtb_post_auction_tests).
rtb_add_library(rtb_post_auction
    SOURCES
        post_auction.cc
    DEPS rtb_banker rtb_common)

rtb_add_area_tests(post_auction DEPS rtb_post_auction)

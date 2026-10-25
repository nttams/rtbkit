# Owner: banker. Add sources to the list; tests go in tests/banker/*_test.cc
# (picked up automatically as rtb_banker_tests).
rtb_add_library(rtb_banker
    SOURCES
        banker.cc
    DEPS rtb_common)

rtb_add_area_tests(banker DEPS rtb_banker)

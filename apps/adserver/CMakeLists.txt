# Owner: post-auction. Standalone ad server + post auction service.
rtb_add_app(adserver
    SOURCES main.cc
    DEPS rtb_adserver rtb_post_auction rtb_banker rtb_common)
# build/apps/adserver is already the CMake directory of this app, so the
# executable gets another file name.
set_target_properties(adserver PROPERTIES OUTPUT_NAME rtb_adserver)

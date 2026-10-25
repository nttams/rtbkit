rtb_add_app(fixed_price_agent
    SOURCES main.cc
    DEPS rtb_bidding_agent)

# rtb_add_app puts the binary in build/apps/<name>, which is also the name of
# this folder's build directory (a directory with the same name), so the link
# fails. Use build/bin instead.
set_target_properties(fixed_price_agent PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")

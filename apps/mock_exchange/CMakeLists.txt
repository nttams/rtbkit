rtb_add_app(mock_exchange
    SOURCES main.cc
    DEPS rtb_exchange)

# build/apps/mock_exchange is already this folder's build directory, so the
# binary can not be written there: put it in build/bin.
set_target_properties(mock_exchange PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")

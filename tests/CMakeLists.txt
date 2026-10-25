include(Catch)

add_executable(rtb_tests
    agent_config_test.cc
    creative_filters_test.cc
    currency_test.cc
    generic_filters_test.cc
    openrtb_parser_test.cc
    static_filters_test.cc
    filter_pool_test.cc
    segments_test.cc)

target_link_libraries(rtb_tests PRIVATE
    rtb_filters rtb_agent_config rtb_common rtb_warnings
    Catch2::Catch2WithMain)
target_compile_definitions(rtb_tests PRIVATE
    RTB_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

catch_discover_tests(rtb_tests)

# Owner: banker. Add sources to the list; tests go in tests/banker/*_test.cc
# (picked up automatically as rtb_banker_tests).
option(RTB_WITH_REDIS "Build RedisBanker (needs hiredis)" OFF)

set(RTB_BANKER_SOURCES banker.cc in_memory_banker.cc banker_factory.cc)
if(RTB_WITH_REDIS)
    find_path(HIREDIS_INCLUDE_DIR hiredis/hiredis.h)
    find_library(HIREDIS_LIBRARY hiredis)
    if(NOT HIREDIS_INCLUDE_DIR OR NOT HIREDIS_LIBRARY)
        message(FATAL_ERROR "RTB_WITH_REDIS=ON but hiredis was not found")
    endif()
    list(APPEND RTB_BANKER_SOURCES redis_banker.cc)
endif()

rtb_add_library(rtb_banker
    SOURCES ${RTB_BANKER_SOURCES}
    DEPS rtb_common)

if(RTB_WITH_REDIS)
    target_compile_definitions(rtb_banker PUBLIC RTB_WITH_REDIS=1)
    target_include_directories(rtb_banker SYSTEM PUBLIC "${HIREDIS_INCLUDE_DIR}")
    target_link_libraries(rtb_banker PUBLIC "${HIREDIS_LIBRARY}")
endif()

rtb_add_area_tests(banker DEPS rtb_banker)

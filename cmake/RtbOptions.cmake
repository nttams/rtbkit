# Common warning flags and a helper that creates our static libraries.

add_library(rtb_warnings INTERFACE)
target_compile_options(rtb_warnings INTERFACE -Wall -Wextra)
option(RTB_WERROR "Treat warnings as errors" OFF)
if(RTB_WERROR)
    target_compile_options(rtb_warnings INTERFACE -Werror)
endif()

# rtb_add_library(<name> SOURCES a.cc b.cc [DEPS other_lib ...])
# Static library, include root is src/ so code includes "common/id.h" etc.
function(rtb_add_library name)
    cmake_parse_arguments(ARG "" "" "SOURCES;DEPS" ${ARGN})
    add_library(${name} STATIC ${ARG_SOURCES})
    target_include_directories(${name} PUBLIC "${PROJECT_SOURCE_DIR}/src")
    target_compile_features(${name} PUBLIC cxx_std_23)
    target_link_libraries(${name} PRIVATE rtb_warnings)
    if(ARG_DEPS)
        target_link_libraries(${name} PUBLIC ${ARG_DEPS})
    endif()
endfunction()

# rtb_add_area_tests(<area> DEPS lib ...)
# One test executable per area, rtb_<area>_tests, built from
# tests/<area>/*_test.cc (globbed, so adding a test file needs no CMake edit).
# Call it from src/<area>/CMakeLists.txt.
function(rtb_add_area_tests area)
    cmake_parse_arguments(ARG "" "" "DEPS" ${ARGN})
    file(GLOB test_sources CONFIGURE_DEPENDS
         "${PROJECT_SOURCE_DIR}/tests/${area}/*_test.cc")
    if(NOT test_sources OR NOT BUILD_TESTING)
        return()
    endif()
    include(Catch)
    set(target rtb_${area}_tests)
    add_executable(${target} ${test_sources})
    target_link_libraries(${target} PRIVATE
        ${ARG_DEPS} rtb_warnings Catch2::Catch2WithMain)
    target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/tests")
    target_compile_definitions(${target} PRIVATE
        RTB_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/data")
    catch_discover_tests(${target})
endfunction()

# rtb_add_app(<name> SOURCES a.cc ... [DEPS lib ...])
# Executable placed in build/apps/. Call it from apps/<name>/CMakeLists.txt.
function(rtb_add_app name)
    cmake_parse_arguments(ARG "" "" "SOURCES;DEPS" ${ARGN})
    add_executable(${name} ${ARG_SOURCES})
    target_compile_features(${name} PRIVATE cxx_std_23)
    target_link_libraries(${name} PRIVATE ${ARG_DEPS} rtb_warnings Threads::Threads)
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/apps")
endfunction()

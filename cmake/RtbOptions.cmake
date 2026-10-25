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

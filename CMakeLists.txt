cmake_minimum_required(VERSION 3.25)

# Homebrew bottles are built for the host macOS version; targeting the same
# version avoids "built for newer macOS" linker warnings.
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    execute_process(COMMAND sw_vers -productVersion
        OUTPUT_VARIABLE RTB_HOST_MACOS OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(RTB_HOST_MACOS)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "${RTB_HOST_MACOS}" CACHE STRING
            "Minimum macOS version")
    endif()
endif()

project(rtbkit
    VERSION 2.0.0
    DESCRIPTION "Modern C++23 RTBKit"
    LANGUAGES CXX)

# Everything lives under build/ (see README); the legacy root Makefile is not
# used by this build.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not supported. Use: cmake -B build -G Ninja")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

include(RtbOptions)
include(RtbDependencies)

add_subdirectory(src)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

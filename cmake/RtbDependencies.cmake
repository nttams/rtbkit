# Homebrew on Apple silicon installs under /opt/homebrew.
if(APPLE AND EXISTS /opt/homebrew)
    list(APPEND CMAKE_PREFIX_PATH /opt/homebrew)
endif()

# Required now.
find_package(nlohmann_json 3.11 REQUIRED)
find_package(Catch2 3 REQUIRED)

# Needed from phase 2 onwards. Pattern: find quietly, expose a RTB_HAVE_<X>
# flag, and let the targets that need them link the imported target.
find_package(cppzmq QUIET)
find_package(httplib QUIET)
find_package(Threads REQUIRED)

foreach(dep cppzmq httplib)
    if(${dep}_FOUND)
        message(STATUS "Optional dependency ${dep}: found")
    else()
        message(STATUS "Optional dependency ${dep}: not found (needed later)")
    endif()
endforeach()

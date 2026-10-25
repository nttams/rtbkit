# Homebrew on Apple silicon installs under /opt/homebrew.
if(APPLE AND EXISTS /opt/homebrew)
    list(APPEND CMAKE_PREFIX_PATH /opt/homebrew)
endif()

# Required now.
find_package(nlohmann_json 3.11 REQUIRED)
find_package(Catch2 3 REQUIRED)

# Messaging and HTTP.
find_package(cppzmq REQUIRED)       # target: cppzmq
find_package(httplib REQUIRED)      # target: httplib::httplib
find_package(Threads REQUIRED)

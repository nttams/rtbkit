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

# Asynchronous HTTP for the exchange connector: standalone asio (header only)
# and llhttp (the HTTP parser of Node.js).
find_path(ASIO_INCLUDE_DIR asio.hpp REQUIRED)
if(NOT TARGET rtb::asio)
    add_library(rtb::asio INTERFACE IMPORTED)
    target_include_directories(rtb::asio INTERFACE "${ASIO_INCLUDE_DIR}")
    target_compile_definitions(rtb::asio INTERFACE ASIO_STANDALONE ASIO_NO_DEPRECATED)
endif()
find_package(llhttp REQUIRED)       # target: llhttp::llhttp_shared

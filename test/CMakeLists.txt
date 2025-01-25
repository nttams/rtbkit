cmake_minimum_required(VERSION 3.10)

project(rtbkit2)

find_package(fmt CONFIG REQUIRED)

add_executable(rtbkit2 main.cc)

target_link_libraries(rtbkit2 PRIVATE fmt::fmt)

set_target_properties(rtbkit2 PROPERTIES CXX_STANDARD 23)
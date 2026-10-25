# Each area owns its own CMakeLists.txt (libraries + its test executable), so
# parallel work in different areas never edits the same CMake file.
add_subdirectory(common)
add_subdirectory(agent)
add_subdirectory(filters)
add_subdirectory(banker)
add_subdirectory(exchange)
add_subdirectory(router)
add_subdirectory(post_auction)
add_subdirectory(adserver)

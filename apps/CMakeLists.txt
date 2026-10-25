# Every apps/<name>/ that has a CMakeLists.txt is built. Each owner creates
# its own folder (use rtb_add_app from cmake/RtbOptions.cmake); this file is
# never edited again.
file(GLOB app_dirs LIST_DIRECTORIES true RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
     "${CMAKE_CURRENT_SOURCE_DIR}/*")
foreach(dir ${app_dirs})
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${dir}/CMakeLists.txt")
        add_subdirectory(${dir})
    endif()
endforeach()

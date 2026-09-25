cmake_minimum_required(VERSION 3.16)

# The published libjpeg-turbo component asks CMake for the include path of a
# target it calls idf::libjpeg-turbo, but the component manager installs it
# under its namespace, so the target is idf::espressif__libjpeg-turbo and the
# generator expression never resolves -- the build dies at generate time.
# Rewritten here because managed_components is not versioned, so a fresh
# checkout downloads the unpatched copy again. The download happens inside
# project(), so the very first build of a clean tree still fails; running it a
# second time picks this up. See README.
set(jpeg_component
    "${CMAKE_CURRENT_LIST_DIR}/managed_components/espressif__libjpeg-turbo/CMakeLists.txt")
if(EXISTS "${jpeg_component}")
    file(READ "${jpeg_component}" jpeg_component_text)
    string(REPLACE "idf::libjpeg-turbo,INCLUDE_DIRECTORIES"
                   "idf::espressif__libjpeg-turbo,INCLUDE_DIRECTORIES"
                   jpeg_component_fixed "${jpeg_component_text}")
    if(NOT jpeg_component_text STREQUAL jpeg_component_fixed)
        file(WRITE "${jpeg_component}" "${jpeg_component_fixed}")
    endif()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(smart_flexispot)

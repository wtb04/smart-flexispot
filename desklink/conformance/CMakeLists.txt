# The panel's own envelope and protocol code with mbedTLS, as the panel has
# them, for testing a Desk Link against. Not part of the firmware build.
cmake_minimum_required(VERSION 3.25)
project(desklink_conformance LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

cmake_path(GET CMAKE_CURRENT_LIST_DIR PARENT_PATH DESKLINK)
cmake_path(GET DESKLINK PARENT_PATH ROOT)
set(LAPTOP ${ROOT}/components/laptop)

include(FetchContent)
FetchContent_Declare(cjson
    URL https://github.com/DaveGamble/cJSON/archive/refs/tags/v1.7.19.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP ON)
FetchContent_Populate(cjson)
add_library(cjson STATIC ${cjson_SOURCE_DIR}/cJSON.c)
target_include_directories(cjson SYSTEM PUBLIC ${cjson_SOURCE_DIR})
target_compile_options(cjson PRIVATE -w)

# ESP-IDF v5.5 bundles mbedTLS 3.6.
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(mbedtls
    URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.4/mbedtls-3.6.4.tar.bz2
    DOWNLOAD_EXTRACT_TIMESTAMP ON)
FetchContent_MakeAvailable(mbedtls)

add_executable(conformance conformance.cpp ${LAPTOP}/link_envelope.cpp ${LAPTOP}/laptop_protocol.cpp)
target_include_directories(conformance PRIVATE ${LAPTOP}/include)
target_link_libraries(conformance PRIVATE cjson mbedcrypto)
target_compile_options(conformance PRIVATE -Wall -Wextra)

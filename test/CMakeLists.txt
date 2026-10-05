# The host-side tests: every component's pure code, the parts that decide and
# parse, built for the desktop and run under GoogleTest. No ESP-IDF needed.
#
#   cmake --workflow --preset host        build and run them
#   cmake --workflow --preset coverage    the same, measured, with a report
cmake_minimum_required(VERSION 3.25)
project(tab5_host_tests LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(TAB5_COVERAGE "Measure which lines the tests reach" OFF)

cmake_path(GET CMAKE_CURRENT_LIST_DIR PARENT_PATH ROOT)
set(COMPONENTS ${ROOT}/components)

include(FetchContent)
FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP ON)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
# The version ESP-IDF v5.5.5 bundles, so the tests parse as the panel does.
FetchContent_Declare(cjson
    URL https://github.com/DaveGamble/cJSON/archive/refs/tags/v1.7.19.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP ON)
FetchContent_MakeAvailable(googletest)
FetchContent_Populate(cjson)  # only its one C file is wanted, not its build

add_library(cjson STATIC ${cjson_SOURCE_DIR}/cJSON.c)
target_include_directories(cjson SYSTEM PUBLIC ${cjson_SOURCE_DIR})
set_target_properties(cjson PROPERTIES C_STANDARD 11)
target_compile_options(cjson PRIVATE -w)  # third party: its warnings are not ours

if(TAB5_COVERAGE)
    set(COVERAGE_FLAGS --coverage -O0 -g)
endif()

enable_testing()
include(GoogleTest)

# host_test(<name> COMPONENT <dir> SOURCES ... [INCLUDES ...] [LIBS ...])
# Sources are relative to components/<dir>; the test sees FIXTURES, the path of
# its component's test folder, for the answers recorded from the real services.
function(host_test name)
    cmake_parse_arguments(T "" "COMPONENT" "SOURCES;INCLUDES;LIBS" ${ARGN})
    set(dir ${COMPONENTS}/${T_COMPONENT})
    list(TRANSFORM T_SOURCES PREPEND ${dir}/)
    add_executable(${name} ${T_SOURCES})
    target_include_directories(${name} PRIVATE ${dir} ${dir}/include ${T_INCLUDES})
    target_link_libraries(${name} PRIVATE GTest::gtest_main ${T_LIBS})
    target_compile_definitions(${name} PRIVATE FIXTURES="${dir}/test")
    target_compile_options(${name} PRIVATE -Wall -Wextra ${COVERAGE_FLAGS})
    target_link_options(${name} PRIVATE ${COVERAGE_FLAGS})
    gtest_discover_tests(${name} TEST_PREFIX "${T_COMPONENT}." DISCOVERY_TIMEOUT 30)
endfunction()

set(UNITS ${COMPONENTS}/units/include)
set(DESKPROTO ${COMPONENTS}/deskproto/include)

host_test(claude_sessions_test COMPONENT claude SOURCES test/test_claude_sessions.cpp claude_sessions.cpp
    LIBS cjson)
host_test(deskproto_test COMPONENT deskproto SOURCES test/test_deskproto.cpp)
host_test(focus_test COMPONENT focus SOURCES test/test_focus.cpp INCLUDES ${UNITS})
host_test(hass_protocol_test COMPONENT hass SOURCES test/test_hass_protocol.cpp hass_protocol.cpp
    INCLUDES ${DESKPROTO} ${UNITS} LIBS cjson)
host_test(ha_ws_protocol_test COMPONENT hass SOURCES test/test_ha_ws_protocol.cpp ha_ws_protocol.cpp
    INCLUDES ${DESKPROTO} ${UNITS} LIBS cjson)
host_test(ical_parse_test COMPONENT ical SOURCES test/test_ical_parse.cpp
    INCLUDES ${COMPONENTS}/rtc/include)
host_test(jellyfin_protocol_test COMPONENT jellyfin SOURCES test/test_jellyfin_protocol.cpp jellyfin_protocol.cpp
    LIBS cjson)
host_test(laptop_protocol_test COMPONENT laptop SOURCES test/test_laptop_protocol.cpp laptop_protocol.cpp
    LIBS cjson)
host_test(job_core_test COMPONENT jobs SOURCES test/test_job_core.cpp job_core.cpp)
host_test(loctek_proto_test COMPONENT loctek SOURCES test/test_loctek_proto.cpp loctek_proto.cpp INCLUDES ${UNITS})
host_test(net_core_test COMPONENT net SOURCES test/test_net_core.cpp net_core.cpp)
host_test(stream_core_test COMPONENT net SOURCES test/test_stream_core.cpp stream_core.cpp)
host_test(gauge_test COMPONENT power SOURCES test/test_gauge.cpp)
host_test(radar_parse_test COMPONENT radar SOURCES test/test_radar_parse.cpp radar_parse.cpp radar_trail.cpp)
host_test(picks_test COMPONENT room SOURCES test/test_picks.cpp picks.cpp LIBS cjson)
host_test(segments_test COMPONENT room SOURCES test/test_segments.cpp segments.cpp LIBS cjson)
host_test(clock_math_test COMPONENT rtc SOURCES test/test_clock_math.cpp)
host_test(travel_parse_test COMPONENT travel SOURCES test/test_travel_parse.cpp)
host_test(watchdog_core_test COMPONENT watchdog SOURCES test/test_watchdog_core.cpp watchdog_core.cpp)

# The report: what the tests reached of each component's own code, the tests
# themselves and what was fetched left out.
if(TAB5_COVERAGE)
    find_program(GCOVR gcovr REQUIRED)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(GCOV_TOOL --gcov-executable "llvm-cov gcov")
    endif()
    add_custom_target(coverage-report
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/coverage
        COMMAND ${GCOVR} --root ${ROOT} ${GCOV_TOOL}
                --filter "${COMPONENTS}/" --exclude "${COMPONENTS}/[^/]+/test/"
                --html-details ${CMAKE_BINARY_DIR}/coverage/index.html
                --xml ${CMAKE_BINARY_DIR}/coverage/coverage.xml
                --markdown ${CMAKE_BINARY_DIR}/coverage/coverage.md
                --txt ${CMAKE_BINARY_DIR}/coverage/coverage.txt
                --print-summary
                ${CMAKE_BINARY_DIR}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Coverage report in ${CMAKE_BINARY_DIR}/coverage/index.html")
endif()

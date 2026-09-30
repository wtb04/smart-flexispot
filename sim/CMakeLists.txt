# The panel's screen in a window on the desktop: the real components/ui, drawn
# by LVGL into SDL, with the hardware and the network stubbed. See sim/README.md.
cmake_minimum_required(VERSION 3.16)
project(smart_flexispot_sim C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 23)  # as ESP-IDF builds the firmware
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(ROOT ${CMAKE_CURRENT_LIST_DIR}/..)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
find_package(ZLIB REQUIRED)  # the screenshots' PNGs
find_package(CURL REQUIRED)  # the live feeds

# The LVGL the firmware builds with: the IDF component manager's copy once a
# firmware build has fetched it, else the same release from GitHub.
set(LV_BUILD_CONF_PATH ${CMAKE_CURRENT_LIST_DIR}/lv_conf.h CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG OFF CACHE BOOL "" FORCE)
set(LV_BUILD_INSTALL OFF CACHE BOOL "" FORCE)
set(LVGL_DIR ${ROOT}/managed_components/lvgl__lvgl CACHE PATH "LVGL's sources")
if(EXISTS ${LVGL_DIR}/lvgl.h)
    add_subdirectory(${LVGL_DIR} lvgl EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(lvgl GIT_REPOSITORY https://github.com/lvgl/lvgl.git GIT_TAG v9.6.0
                         GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(lvgl)
endif()
target_link_libraries(lvgl PUBLIC PkgConfig::SDL2)

file(GLOB UI_SOURCES ${ROOT}/components/ui/*.cpp ${ROOT}/components/ui/fonts/*.c
     ${ROOT}/components/ui/fonts/*.cpp)
file(GLOB SIM_SOURCES ${CMAKE_CURRENT_LIST_DIR}/*.cpp ${CMAKE_CURRENT_LIST_DIR}/stubs/*.cpp
     ${CMAKE_CURRENT_LIST_DIR}/live/*.cpp)
# What the home page holds, from the room component that fills it on the panel.
set(ROOM_LAYOUT ${ROOT}/components/room/room_layout.cpp)

# The panel's own parsers, which are plain C++ and run here as there: the
# feeds', and Home Assistant's for its notices, with the cJSON ESP-IDF carries.
if(DEFINED ENV{IDF_PATH})
    set(IDF_DIR $ENV{IDF_PATH})
else()
    set(IDF_DIR $ENV{HOME}/esp/esp-idf)
endif()
set(CJSON_DIR ${IDF_DIR}/components/json/cJSON)
if(NOT EXISTS ${CJSON_DIR}/cJSON.c)  # no ESP-IDF here, as in CI: the version it bundles
    include(FetchContent)
    FetchContent_Declare(cjson URL https://github.com/DaveGamble/cJSON/archive/refs/tags/v1.7.19.tar.gz
                         DOWNLOAD_EXTRACT_TIMESTAMP ON)
    FetchContent_Populate(cjson)
    set(CJSON_DIR ${cjson_SOURCE_DIR})
endif()
set_source_files_properties(${CJSON_DIR}/cJSON.c PROPERTIES COMPILE_OPTIONS -w)
set(NET_CORE ${ROOT}/components/net/net_core.cpp)  # the panel's own, under net_desktop.cpp
set(PARSERS ${ROOT}/components/ical/ical_parse.cpp ${ROOT}/components/travel/travel_parse.cpp
    ${ROOT}/components/radar/radar_parse.cpp ${ROOT}/components/radar/radar_trail.cpp
    ${ROOT}/components/hass/hass_protocol.cpp ${CJSON_DIR}/cJSON.c)

add_executable(sim ${SIM_SOURCES} ${UI_SOURCES} ${ROOM_LAYOUT} ${PARSERS} ${NET_CORE})
target_include_directories(sim PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/shim
    ${ROOT}/components/ui ${ROOT}/components/ui/include ${ROOT}/components/ui/fonts
    ${ROOT}/components/board/include
    ${ROOT}/components/deskproto/include
    ${ROOT}/components/focus/include
    ${ROOT}/components/hass/include
    ${ROOT}/components/ical ${ROOT}/components/ical/include
    ${ROOT}/components/net/include
    ${ROOT}/components/media/include
    ${ROOT}/components/radar ${ROOT}/components/radar/include
    ${ROOT}/components/room
    ${ROOT}/components/rtc/include
    ${ROOT}/components/travel ${ROOT}/components/travel/include
    ${ROOT}/components/units/include
    ${CJSON_DIR})
target_link_libraries(sim PRIVATE lvgl ZLIB::ZLIB CURL::libcurl)
if(APPLE)
    target_link_libraries(sim PRIVATE "-framework CoreFoundation" "-framework CoreGraphics" "-framework ImageIO")
endif()

# `cmake --build sim/build --target screenshots`: the main views, offline and
# the same each time, into sim/build/screenshots, as CI keeps them.
set(SCENES
    "home|--press amm --shot 2"
    "radar|--page 1 --shot 2"
    "calendar|--page 2 --shot 2"
    "focus|--page 3 --shot 2"
    "setup|--page 4 --shot 2"
    "diagnostics|--page 4 --tap 800,310 --shot 3"
    "cinema|--press amm --tap 1200,548 --shot 3"
    "notice|--press amm --tap 1200,548 --then n --shot 4"
    "splash|--splash --shot 5")
set(SHOTS ${CMAKE_BINARY_DIR}/screenshots)
set(SHOT_COMMANDS COMMAND ${CMAKE_COMMAND} -E make_directory ${SHOTS})
foreach(scene IN LISTS SCENES)
    string(REPLACE "|" ";" parts "${scene}")
    list(GET parts 0 name)
    list(GET parts 1 args)
    separate_arguments(args UNIX_COMMAND "${args}")
    list(APPEND SHOT_COMMANDS COMMAND ${CMAKE_COMMAND} -E env SIM_OFFLINE=1
         $<TARGET_FILE:sim> ${args} --out ${SHOTS}/${name}.png)
endforeach()
add_custom_target(screenshots ${SHOT_COMMANDS}
    DEPENDS sim WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR} VERBATIM
    COMMENT "Screenshots of the main views in ${SHOTS}")

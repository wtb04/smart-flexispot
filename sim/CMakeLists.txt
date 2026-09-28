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
# The feeds' own parsers, which are plain C++ and run here as on the panel.
# What the home page holds, from the room component that fills it on the panel.
set(ROOM_LAYOUT ${ROOT}/components/room/room_layout.cpp)
set(PARSERS ${ROOT}/components/ical/ical_parse.cpp ${ROOT}/components/travel/travel_parse.cpp
    ${ROOT}/components/radar/radar_parse.cpp ${ROOT}/components/radar/radar_trail.cpp)

add_executable(sim ${SIM_SOURCES} ${UI_SOURCES} ${ROOM_LAYOUT} ${PARSERS})
target_include_directories(sim PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/shim
    ${ROOT}/components/ui ${ROOT}/components/ui/include ${ROOT}/components/ui/fonts
    ${ROOT}/components/board/include
    ${ROOT}/components/deskproto/include
    ${ROOT}/components/focus/include
    ${ROOT}/components/hass/include
    ${ROOT}/components/ical ${ROOT}/components/ical/include
    ${ROOT}/components/media/include
    ${ROOT}/components/radar ${ROOT}/components/radar/include
    ${ROOT}/components/room
    ${ROOT}/components/rtc/include
    ${ROOT}/components/travel ${ROOT}/components/travel/include
    ${ROOT}/components/units/include)
target_link_libraries(sim PRIVATE lvgl ZLIB::ZLIB CURL::libcurl)
if(APPLE)
    target_link_libraries(sim PRIVATE "-framework CoreFoundation" "-framework CoreGraphics" "-framework ImageIO")
endif()

file(GLOB UI_FONTS "fonts/lv_font_*.c")
include(${CMAKE_CURRENT_LIST_DIR}/fronts.cmake)
# core: the state handed over and what follows it; kit: the design kit; lab: its
# test bench; classic and front: the two screens, one of which goes in a build.
file(GLOB UI_CORE_SOURCES "core/*.cpp" "kit/*.cpp" "lab/*.cpp")
list(APPEND UI_CORE_SOURCES fonts/units_font.cpp fonts/kit_fonts.cpp)
if(CONFIG_UI_KIT)
    set(UI_FRONT_SOURCES ${UI_KIT_SOURCES})
else()
    set(UI_FRONT_SOURCES ${UI_CLASSIC_SOURCES})
endif()
idf_component_register(
    SRCS ${UI_CORE_SOURCES} ${UI_FRONT_SOURCES} ${UI_FONTS}
    INCLUDE_DIRS "include" "fonts"
    PRIV_INCLUDE_DIRS "." "core" "kit" "front" "classic" "lab"
    REQUIRES claude esp_app_format esp_driver_ppa lvgl__lvgl espressif__esp_lvgl_port board media radar ical travel deskproto units watchdog
)

# The allocator is only referred to by LVGL, which links after this component;
# without this the linker has passed it by before anything asks for it.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_mem_init")

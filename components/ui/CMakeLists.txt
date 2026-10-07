file(GLOB UI_FONTS "fonts/lv_font_*.c")
# core: the state handed over and what follows it; kit: the design kit; lab: its
# test bench; front: the screen, built from the kit.
file(GLOB UI_SOURCES "core/*.cpp" "kit/*.cpp" "lab/*.cpp" "front/*.cpp")
idf_component_register(
    SRCS ${UI_SOURCES} fonts/kit_fonts.cpp ${UI_FONTS}
    INCLUDE_DIRS "include" "fonts"
    PRIV_INCLUDE_DIRS "." "core" "kit" "front" "lab"
    REQUIRES claude esp_driver_ppa lvgl__lvgl espressif__esp_lvgl_port board media radar ical travel deskproto units watchdog
)

# The allocator is only referred to by LVGL, which links after this component;
# without this the linker has passed it by before anything asks for it.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_mem_init")

file(GLOB UI_FONTS "fonts/lv_font_*.c")
idf_component_register(
    SRCS "ui.cpp" "rail.cpp" "notify.cpp" "home_page.cpp" "nav.cpp" "diagnostics_page.cpp" "settings_page.cpp" "splash.cpp" "lv_mem_psram.c" "radar_page.cpp" "calendar_page.cpp" "focus_page.cpp" "screenshot.cpp" "segment_display.cpp" "modal_overlay.cpp" "fonts/units_font.cpp" ${UI_FONTS}
    INCLUDE_DIRS "include" "fonts"
    REQUIRES esp_app_format lvgl__lvgl espressif__esp_lvgl_port board media radar ical travel deskproto
)

# The allocator is only referred to by LVGL, which links after this component;
# without this the linker has passed it by before anything asks for it.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_mem_init")

file(GLOB UI_FONTS "fonts/lv_font_*.c")
idf_component_register(
    SRCS "ui.cpp" "radar_page.cpp" "calendar_page.cpp" "segment_display.cpp" "modal_overlay.cpp" "fonts/units_font.cpp" ${UI_FONTS}
    INCLUDE_DIRS "include" "fonts"
    REQUIRES lvgl__lvgl espressif__esp_lvgl_port board media radar ical travel
)

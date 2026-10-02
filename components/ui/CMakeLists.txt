file(GLOB UI_FONTS "fonts/lv_font_*.c")
idf_component_register(
    SRCS "ui.cpp" "rail.cpp" "notify.cpp" "home_page.cpp" "nav.cpp" "diagnostics_page.cpp" "settings_page.cpp" "splash.cpp" "lv_mem_psram.cpp" "radar_page.cpp" "calendar_page.cpp" "focus_page.cpp" "screenshot.cpp" "segment_display.cpp" "speaker_art.cpp" "cinema.cpp" "music.cpp" "fullscreen_chrome.cpp" "topics.cpp" "focus_model.cpp" "media_model.cpp" "room_model.cpp" "status_model.cpp" "modal_overlay.cpp" "views.cpp" "fonts/units_font.cpp" ${UI_FONTS}
    INCLUDE_DIRS "include" "fonts"
    REQUIRES esp_app_format esp_driver_ppa lvgl__lvgl espressif__esp_lvgl_port board media radar ical travel deskproto units
)

# The allocator is only referred to by LVGL, which links after this component;
# without this the linker has passed it by before anything asks for it.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_mem_init")

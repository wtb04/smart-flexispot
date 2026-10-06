file(GLOB UI_FONTS "fonts/lv_font_*.c")
include(${CMAKE_CURRENT_LIST_DIR}/fronts.cmake)
set(UI_CORE_SOURCES
    ui.cpp lv_mem_psram.cpp screenshot.cpp topics.cpp focus_model.cpp media_model.cpp room_model.cpp home_model.cpp
    settings_model.cpp diagnostics_model.cpp radar_model.cpp notices_model.cpp screen_schedule.cpp status_model.cpp views.cpp wall_clock.cpp
    kit.cpp kit_bake.cpp kit_instruments.cpp kit_popover.cpp kit_perf.cpp kit_pages.cpp kit_pictures.cpp kit_lists.cpp
    kit_readings.cpp lab.cpp lab_lists.cpp lab_pages.cpp lab_pictures.cpp fonts/units_font.cpp fonts/kit_fonts.cpp)
if(CONFIG_UI_KIT)
    set(UI_FRONT_SOURCES ${UI_KIT_SOURCES})
else()
    set(UI_FRONT_SOURCES ${UI_CLASSIC_SOURCES})
endif()
idf_component_register(
    SRCS ${UI_CORE_SOURCES} ${UI_SHARED_SOURCES} ${UI_FRONT_SOURCES} ${UI_FONTS}
    INCLUDE_DIRS "include" "fonts"
    REQUIRES claude esp_app_format esp_driver_ppa lvgl__lvgl espressif__esp_lvgl_port board media radar ical travel deskproto units
)

# The allocator is only referred to by LVGL, which links after this component;
# without this the linker has passed it by before anything asks for it.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_mem_init")

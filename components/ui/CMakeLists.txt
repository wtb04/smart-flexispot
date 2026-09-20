idf_component_register(
    SRCS "ui.cpp" "segment_display.cpp"
    INCLUDE_DIRS "include"
    REQUIRES lvgl__lvgl espressif__esp_lvgl_port board
)

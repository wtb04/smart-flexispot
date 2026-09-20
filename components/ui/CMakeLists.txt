idf_component_register(
    SRCS "ui.cpp"
    INCLUDE_DIRS "include"
    REQUIRES lvgl__lvgl espressif__esp_lvgl_port board
)

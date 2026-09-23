idf_component_register(
    SRCS "main.cpp" "link.cpp" "leds.cpp"
    INCLUDE_DIRS "."
    REQUIRES loctek deskproto nvs_flash esp_timer bt driver
)

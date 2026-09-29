idf_component_register(
    SRCS "ical.cpp" "ical_parse.cpp"
    INCLUDE_DIRS "include" "."
    REQUIRES rtc
    PRIV_REQUIRES net nvs_flash esp_timer wifi units
)

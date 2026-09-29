idf_component_register(
    SRCS "radar.cpp" "radar_parse.cpp" "radar_trail.cpp"
    INCLUDE_DIRS "include" "."
    PRIV_REQUIRES net jpeg units esp_timer
)

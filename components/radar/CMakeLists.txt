idf_component_register(
    SRCS "radar.cpp" "radar_parse.cpp" "radar_trail.cpp"
    INCLUDE_DIRS "include" "."
    PRIV_REQUIRES net jobs app_state jpeg units esp_timer
)

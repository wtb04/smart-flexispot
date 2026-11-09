idf_component_register(
    SRCS "weather.cpp" "weather_parse.cpp"
    INCLUDE_DIRS "include" "."
    PRIV_REQUIRES net jobs esp_timer units
)

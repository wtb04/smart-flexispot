idf_component_register(
    SRCS "travel.cpp" "travel_parse.cpp"
    INCLUDE_DIRS "include" "."
    PRIV_REQUIRES net esp_timer wifi units
)

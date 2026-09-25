idf_component_register(
    SRCS "travel.cpp" "travel_parse.cpp"
    INCLUDE_DIRS "include" "."
    PRIV_REQUIRES esp_http_client esp-tls esp_timer wifi units
)

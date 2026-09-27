idf_component_register(
    SRCS "radar.cpp" "radar_parse.cpp" "radar_trail.cpp"
    INCLUDE_DIRS "include" "."
    REQUIRES esp_http_client esp-tls mbedtls esp_timer jpeg
    PRIV_REQUIRES units
)

idf_component_register(
    SRCS "radar.cpp" "radar_parse.cpp"
    INCLUDE_DIRS "include" "."
    REQUIRES esp_http_client esp-tls mbedtls esp_timer media
)

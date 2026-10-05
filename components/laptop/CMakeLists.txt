idf_component_register(
    SRCS "laptop_protocol.cpp" "link_envelope.cpp" "laptop.cpp"
    INCLUDE_DIRS "include"
    REQUIRES json
    PRIV_REQUIRES net ota units esp_http_server esp_timer lwip mbedtls esp_app_format
)

idf_component_register(
    SRCS "ota.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server
    PRIV_REQUIRES app_update esp_app_format esp_timer esp_partition settings mbedtls deskproto units
)

idf_component_register(
    SRCS "jellyfin_protocol.cpp" "jellyfin.cpp"
    INCLUDE_DIRS "include"
    REQUIRES json
    PRIV_REQUIRES espressif__esp_websocket_client net esp-tls mbedtls units
)

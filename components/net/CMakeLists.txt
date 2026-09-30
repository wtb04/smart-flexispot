idf_component_register(
    SRCS "net.cpp" "net_core.cpp" "net_stream.cpp" "stream_core.cpp"
    INCLUDE_DIRS "include"
    REQUIRES mqtt
    PRIV_REQUIRES esp_http_client espressif__esp_websocket_client esp-tls mbedtls esp_timer units app_state watchdog
)

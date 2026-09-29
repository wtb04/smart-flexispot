idf_component_register(
    SRCS "net.cpp" "net_core.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_http_client esp-tls mbedtls esp_timer units wifi
)

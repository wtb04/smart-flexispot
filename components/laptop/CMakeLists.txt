idf_component_register(
    SRCS "laptop_protocol.cpp" "laptop.cpp"
    INCLUDE_DIRS "include"
    REQUIRES json
    PRIV_REQUIRES net ota units esp_http_server esp_timer lwip
)

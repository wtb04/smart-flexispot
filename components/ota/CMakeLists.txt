idf_component_register(
    SRCS "ota.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_http_server app_update esp_app_format esp_timer deskproto units
)

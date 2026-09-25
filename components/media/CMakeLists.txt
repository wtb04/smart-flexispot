idf_component_register(
    SRCS "media.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client esp_timer jpeg
    PRIV_REQUIRES units
)

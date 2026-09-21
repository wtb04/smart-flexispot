idf_component_register(
    SRCS "media.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_jpeg esp_http_client hass espressif__libjpeg-turbo
)

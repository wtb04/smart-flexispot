idf_component_register(
    SRCS "jpeg.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_jpeg espressif__libjpeg-turbo units
)

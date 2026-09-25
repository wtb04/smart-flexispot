idf_component_register(
    SRCS "settings.cpp"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_timer
    PRIV_REQUIRES units
)

idf_component_register(
    SRCS "media.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer jpeg
    PRIV_REQUIRES net units
)

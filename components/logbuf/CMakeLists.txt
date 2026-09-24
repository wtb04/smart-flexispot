idf_component_register(
    SRCS "logbuf.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_common log esp_timer
    PRIV_REQUIRES rtc
)

idf_component_register(
    SRCS "watchdog.cpp" "watchdog_core.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_timer
)

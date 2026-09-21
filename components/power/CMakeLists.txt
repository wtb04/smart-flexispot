idf_component_register(
    SRCS "power.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer espressif__m5stack_tab5 espressif__esp_io_expander esp_driver_i2c
)

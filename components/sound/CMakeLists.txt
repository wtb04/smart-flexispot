idf_component_register(
    SRCS "sound.cpp"
    INCLUDE_DIRS "include"
    REQUIRES espressif__m5stack_tab5 espressif__esp_codec_dev
    PRIV_REQUIRES units
)

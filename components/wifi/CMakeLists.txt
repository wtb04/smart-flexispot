idf_component_register(
    SRCS "wifi.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event nvs_flash lwip rtc espressif__m5stack_tab5
    PRIV_REQUIRES units app_state
)

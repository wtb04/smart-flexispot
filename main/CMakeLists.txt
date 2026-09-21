idf_component_register(
    SRCS "main.cpp" "desk.cpp" "battery.cpp" "wallclock.cpp" "telemetry.cpp" "diagnostics.cpp" "room.cpp"
    INCLUDE_DIRS "."
    REQUIRES settings logbuf radar ble media board ui loctek power wifi hass sound esp_wifi esp_netif esp_timer nvs_flash esp_app_format
)

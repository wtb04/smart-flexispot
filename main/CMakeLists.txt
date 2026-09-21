idf_component_register(
    SRCS "main.cpp" "desk.cpp" "battery.cpp" "wallclock.cpp" "telemetry.cpp" "room.cpp"
    INCLUDE_DIRS "."
    REQUIRES ble media board ui loctek power wifi hass sound esp_wifi esp_netif esp_timer
)

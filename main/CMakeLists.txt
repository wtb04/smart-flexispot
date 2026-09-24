idf_component_register(
    SRCS "main.cpp" "desk.cpp" "battery.cpp" "wallclock.cpp" "telemetry.cpp" "diagnostics.cpp" "room.cpp"
    INCLUDE_DIRS "."
    REQUIRES settings logbuf radar ble media board ui loctek power rtc wifi hass sound ical travel esp_timer nvs_flash esp_app_format
)

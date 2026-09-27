idf_component_register(
    SRCS "main.cpp" "battery.cpp" "wallclock.cpp" "network.cpp" "diagnostics.cpp" "orientation.cpp" "remote.cpp"
    INCLUDE_DIRS "."
    REQUIRES desk focus ota room imu settings logbuf radar ble media jpeg board ui loctek power rtc wifi hass jellyfin sound ical travel units esp_timer nvs_flash esp_app_format
)

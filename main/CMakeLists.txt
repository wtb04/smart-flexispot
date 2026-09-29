idf_component_register(
    SRCS "main.cpp" "battery.cpp" "wallclock.cpp" "network.cpp" "diagnostics.cpp" "orientation.cpp" "remote.cpp" "last_words.cpp"
    INCLUDE_DIRS "."
    REQUIRES net desk focus ota room imu settings logbuf radar ble media jpeg board ui loctek power rtc wifi hass jellyfin sound ical travel units esp_timer nvs_flash esp_app_format esp_system riscv
)
# last_words.cpp notes a crash before the panic handler goes on with it.
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")

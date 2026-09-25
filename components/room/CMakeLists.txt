idf_component_register(
    SRCS "room.cpp" "picks.cpp" "segments.cpp"
    INCLUDE_DIRS "include"
    REQUIRES hass ui media radar rtc esp_timer json esp_http_client jellyfin
    PRIV_REQUIRES units mbedtls
)

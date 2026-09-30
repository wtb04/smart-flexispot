idf_component_register(
    SRCS "room.cpp" "room_layout.cpp" "picks.cpp" "segments.cpp"
    INCLUDE_DIRS "include"
    REQUIRES hass ui media radar rtc esp_timer json jellyfin
    PRIV_REQUIRES net jobs units
)

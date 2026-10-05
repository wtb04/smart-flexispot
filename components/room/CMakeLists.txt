idf_component_register(
    SRCS "room.cpp" "room_layout.cpp" "picks.cpp" "segments.cpp" "ha_values.cpp"
         "media_card.cpp" "speaker_player.cpp" "jellyfin_player.cpp" "laptop_player.cpp"
    INCLUDE_DIRS "include"
    REQUIRES hass ui media radar rtc esp_timer json jellyfin laptop
    PRIV_REQUIRES net jobs units
)

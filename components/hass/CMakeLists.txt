idf_component_register(
    SRCS "hass_protocol.cpp" "hass.cpp"
    INCLUDE_DIRS "include"
    REQUIRES mqtt json esp_wifi esp_netif
)

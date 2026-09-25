idf_component_register(
    SRCS "hass_protocol.cpp" "hass.cpp" "ha_ws_protocol.cpp" "ha_ws.cpp"
    INCLUDE_DIRS "include"
    REQUIRES mqtt json esp_wifi esp_netif espressif__esp_websocket_client deskproto
    PRIV_REQUIRES units
)

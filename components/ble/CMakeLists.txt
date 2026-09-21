idf_component_register(
    SRCS "ble.cpp" "proxy_link.cpp"
    INCLUDE_DIRS "include"
    REQUIRES bt mbedtls deskproto
)

idf_component_register(
    SRCS "desk.cpp"
    INCLUDE_DIRS "include"
    REQUIRES loctek ble deskproto nvs_flash
)

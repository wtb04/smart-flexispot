idf_component_register(
    SRCS "desk.cpp" "wire.cpp" "bluetooth.cpp"
    INCLUDE_DIRS "include"
    REQUIRES loctek ble deskproto settings
)

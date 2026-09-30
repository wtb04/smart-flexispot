idf_component_register(
    SRCS "jellyfin_protocol.cpp" "jellyfin.cpp"
    INCLUDE_DIRS "include"
    REQUIRES json
    PRIV_REQUIRES net units
)

idf_component_register(
    SRCS "loctek.cpp" "loctek_proto.cpp"
    INCLUDE_DIRS "include"
    REQUIRES driver nvs_flash
    PRIV_REQUIRES units
)

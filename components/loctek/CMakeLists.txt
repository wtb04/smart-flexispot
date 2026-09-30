idf_component_register(
    SRCS "loctek.cpp" "loctek_proto.cpp"
    INCLUDE_DIRS "include"
    REQUIRES driver
    PRIV_REQUIRES settings units
)

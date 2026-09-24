idf_component_register(
    SRCS "imu.cpp"
    INCLUDE_DIRS "include"
    REQUIRES espressif__bmi270 espressif__m5stack_tab5
)

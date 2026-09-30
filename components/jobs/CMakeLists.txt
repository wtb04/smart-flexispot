idf_component_register(
    SRCS "jobs.cpp" "job_core.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES app_state watchdog esp_timer
)

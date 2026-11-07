idf_component_register(
    SRCS "rtos.cpp" "retained_uptime.cpp" "common.cpp" "ag_i2c.cpp" "air_quality.cpp"
         "measurement_corrections.cpp"
    INCLUDE_DIRS "include"
    REQUIRES freertos esp_timer esp_system heap airgradient-gpio esp_app_format driver
    PRIV_REQUIRES esp_common esp_hw_support
)

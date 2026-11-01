idf_component_register(
    SRCS "drivers/native_gpio.cpp" "drivers/tca6408a.cpp"
    INCLUDE_DIRS "hal" "drivers"
    REQUIRES esp_driver_gpio esp_driver_i2c airgradient-common
)

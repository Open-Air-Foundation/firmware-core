idf_component_register(
    SRCS "drivers/sht40/sht40.cpp"
         "drivers/pms5003/pms5003_base.cpp"
         "drivers/pms5003/pms5003.cpp"
         "drivers/pms5003/pms5003t.cpp"
         "drivers/sps30/sps30.cpp"
         "drivers/co2_common/modbus_crc.cpp"
         "drivers/s8/s8.cpp"
         "drivers/sunlight/sunlight.cpp"
         "drivers/stcc4/stcc4.cpp"
         "drivers/scd4x/scd4x.cpp"
         "drivers/s12/s12.cpp"
         "drivers/sgp41/sgp41.cpp"
         "drivers/alpha_sense/alpha_sense.cpp"
         "drivers/dps368/dps368.cpp"
         "drivers/spl07/spl07.cpp"
         "services/sensor_manager.cpp"
    INCLUDE_DIRS "."
    REQUIRES airgradient-common airgradient-serial ads1115 embedded-i2c-scd4x esp_driver_gpio esp_driver_i2c
             sensirion-gas-index-algorithm
)

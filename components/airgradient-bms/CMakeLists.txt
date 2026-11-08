idf_component_register(
    SRCS "drivers/bq25xx/bq25xx.cpp"
         "drivers/bq25629/bq25629_bms.cpp"
         "drivers/bq27427/bq27427.cpp"
         "drivers/bq27742/bq27742.cpp"
    INCLUDE_DIRS "."
    REQUIRES airgradient-common esp_driver_i2c bq25629
)

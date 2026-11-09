idf_component_register(
    SRCS "drivers/spi_nand_storage.cpp"
    INCLUDE_DIRS "hal" "drivers"
    REQUIRES spi_nand_flash airgradient-gpio
    PRIV_REQUIRES esp_driver_spi
)

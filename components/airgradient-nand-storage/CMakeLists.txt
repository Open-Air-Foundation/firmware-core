idf_component_register(
    SRCS "drivers/spi_nand_storage.cpp"
    INCLUDE_DIRS "hal" "drivers"
    REQUIRES spi_nand_flash airgradient-gpio
    PRIV_REQUIRES esp_driver_spi
)

# The NAND library has no task-context CS hook. SPI pre/post callbacks run in
# an ISR and cannot drive an I2C expander. Wrap its synchronous transport call;
# devices without software CS pass straight through to ESP-IDF.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=spi_device_transmit")

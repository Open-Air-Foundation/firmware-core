idf_component_register(
    SRCS "drivers/sd_card_storage.cpp"
         "drivers/sd_card_selftest.cpp"
         "drivers/sd_log_capture.cpp"
    INCLUDE_DIRS "drivers"
    REQUIRES airgradient-gpio esp_driver_sdspi sdmmc fatfs
    PRIV_REQUIRES esp_driver_spi esp_timer esp_ringbuf
)

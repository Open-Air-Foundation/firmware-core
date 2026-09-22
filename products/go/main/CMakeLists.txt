idf_component_register(
    SRCS "main.cpp"
         "go_app.cpp"
         "go_hardware_board.cpp"
         "go_ble.cpp"
         "go_cloud.cpp"
         "go_types.cpp"
         "go_settings.cpp"
         "go_display.cpp"
         "gps/gps_driver.cpp"
         "gps/gps_service.cpp"
         "go_input.cpp"
         "go_local_api.cpp"
         "go_orchestrator.cpp"
         "go_ota.cpp"
         "go_portable_provisioner.cpp"
         "go_power.cpp"
         "go_sensor_producer.cpp"
         "serial_command/serial_command.cpp"
         "serial_command/serial_command_usb.cpp"
         "go_storage.cpp"
         "go_ui.cpp"
         "go_ulp.cpp"
         "go_text_wrap.cpp"
         "go_wifi.cpp"
         "led/go_led.cpp"
         "led/go_led_driver.cpp"
         "buzzer/go_buzzer.cpp"
         "buzzer/go_buzzer_driver.cpp"
         "accel/lis2dh12.cpp"
         "accel/accel_service.cpp"
         "accel/shake_detector.cpp"
         "go_melody_sync.cpp"
         "fg_learning/fg_learning_controller.cpp"
         "fg_learning/fg_learning_runner.cpp"
         "fg_learning/fg_learning_journal.cpp"
    INCLUDE_DIRS "." "led" "buzzer" "accel" "fg_learning"
    REQUIRES airgradient-bms
             bq25629
    REQUIRES airgradient-ble
             airgradient-client
             airgradient-common
             airgradient-config
             airgradient-ota
              airgradient-gpio
              airgradient-http-server
              airgradient-local-server
              airgradient-nand-storage
              airgradient-provisioning
              airgradient-serial
              airgradient-wifi
              libnmea-esp32
              airgradient-payload-cache
              airgradient-sensors
              airgradient-touch
              json
               esp_driver_gpio
              esp_driver_i2c
              esp_driver_ledc
              esp_driver_spi
              esp_driver_usb_serial_jtag
             nvs_flash
             u8g2
             fatfs
             ulp
)

ulp_embed_binary(ulp_wdt "ulp/wdt_feed.c" "")

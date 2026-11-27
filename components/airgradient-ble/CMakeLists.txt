idf_component_register(
    SRCS "drivers/nimble_ble_server.cpp"
         "services/ble_device_information.cpp"
    INCLUDE_DIRS "." "drivers"
    REQUIRES esp-nimble-cpp
)

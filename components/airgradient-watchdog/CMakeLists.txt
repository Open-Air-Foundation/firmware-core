idf_component_register(
    SRCS "airgradient_watchdog.cpp"
    INCLUDE_DIRS "include"
    REQUIRES airgradient-gpio
    PRIV_REQUIRES airgradient-common
)

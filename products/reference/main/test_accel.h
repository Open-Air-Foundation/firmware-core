#ifndef TEST_ACCEL_H
#define TEST_ACCEL_H

#include "airgradient_gpio.h"
#include "driver/i2c_master.h"

// Awake LIS2DH12 interrupt/gesture diagnostic for Go hardware. Runs until reset.
void run_test_accel(i2c_master_bus_handle_t bus, const gpio::Hal &gpio);

#endif // TEST_ACCEL_H

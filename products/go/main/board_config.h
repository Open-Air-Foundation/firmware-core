/**
 * AirGradient Go — Board Configuration
 *
 * Pin assignments and peripheral constants for the AGo board.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#include <cstdint>

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <driver/uart.h>

// ---------------------------------------------------------------------------
// SPI bus (shared by display and NAND flash)
//
// Both the e-paper display and NAND flash sit on the same SPI2 bus with
// individual chip-select lines.  The bus is initialised once; each driver
// adds its own device via spi_bus_add_device().
// ---------------------------------------------------------------------------

inline constexpr spi_host_device_t SPI_HOST = SPI2_HOST;
inline constexpr gpio_num_t PIN_SPI_MOSI = GPIO_NUM_25;
inline constexpr gpio_num_t PIN_SPI_MISO = GPIO_NUM_24;
inline constexpr gpio_num_t PIN_SPI_SCLK = GPIO_NUM_23;

// ---------------------------------------------------------------------------
// I2C bus
// ---------------------------------------------------------------------------

inline constexpr gpio_num_t PIN_I2C_SCL = GPIO_NUM_6;
inline constexpr gpio_num_t PIN_I2C_SDA = GPIO_NUM_7;
inline constexpr i2c_port_num_t I2C_MASTER_PORT = I2C_NUM_0;
inline constexpr int I2C_GLITCH_IGNORE_CNT = 7;
inline constexpr bool I2C_INTERNAL_PULLUPS = true;

// ---------------------------------------------------------------------------
// Display — SSD1680 e-paper (SPI, shared bus)
// ---------------------------------------------------------------------------

inline constexpr gpio_num_t PIN_DISPLAY_CS = GPIO_NUM_0;
inline constexpr gpio_num_t PIN_DISPLAY_DC = GPIO_NUM_15;
inline constexpr gpio_num_t PIN_DISPLAY_RST = GPIO_NUM_9;
inline constexpr gpio_num_t PIN_DISPLAY_BUSY = GPIO_NUM_10;

// ---------------------------------------------------------------------------
// NAND flash (SPI, shared bus)
// ---------------------------------------------------------------------------

inline constexpr gpio_num_t PIN_NAND_CS = GPIO_NUM_4;

// ---------------------------------------------------------------------------
// UART — GPS (NmeaGps)
// ---------------------------------------------------------------------------

inline constexpr uart_port_t UART_PORT_GPS = UART_NUM_1;
inline constexpr gpio_num_t PIN_GPS_TX = GPIO_NUM_11;
inline constexpr gpio_num_t PIN_GPS_RX = GPIO_NUM_12;
inline constexpr int GPS_BAUD = 115200;

// ---------------------------------------------------------------------------
// I2C device addresses
// ---------------------------------------------------------------------------

inline constexpr uint8_t I2C_ADDR_S12 = 0x68;      // SenseAir S12 CO2
inline constexpr uint8_t I2C_ADDR_SCD4X = 0x62;    // Sensirion SCD4x CO2 + T/RH
inline constexpr uint8_t I2C_ADDR_STCC4 = 0x64;    // Sensirion STCC4 CO2 + T/RH
inline constexpr uint8_t I2C_ADDR_SHT40 = 0x44;    // Dedicated temp/humidity (V1)
inline constexpr uint8_t I2C_ADDR_SGP41 = 0x59;    // TVOC & NOx
inline constexpr uint8_t I2C_ADDR_DPS368 = 0x77;   // Pressure + altitude
inline constexpr uint8_t I2C_ADDR_BMS = 0x6A;      // BQ25629 battery charger
inline constexpr uint8_t I2C_ADDR_CAP1203 = 0x28;  // Capacitive touch
inline constexpr uint8_t I2C_ADDR_LIS2DH12 = 0x18; // ST accelerometer (SA0 = GND)
// SPS30 PM sensor uses a fixed address (0x69) defined in the driver.

// ---------------------------------------------------------------------------
// PM sensor power enable (SPS30, I2C)
//
// Prototype boards use active-high (level 1 = PM on).
// v1 boards use active-low   (level 0 = PM on).
// The runtime variant detection in init_buses() selects the correct level.
// ---------------------------------------------------------------------------

inline constexpr gpio_num_t PIN_PM_POWER = GPIO_NUM_26;
inline constexpr uint8_t PM_POWER_ON_LEVEL_PROTOTYPE = 1; ///< Active-high
inline constexpr uint8_t PM_POWER_ON_LEVEL_V1 = 0;        ///< Active-low

// ---------------------------------------------------------------------------
// Buzzer — HYG-8503A (LEDC PWM via NPN low-side switch Q3)
// ---------------------------------------------------------------------------

inline constexpr gpio_num_t PIN_BUZZER = GPIO_NUM_8; ///< EN_BUZZ net
inline constexpr uint32_t BUZZER_FREQ_HZ = 2700;     ///< HYG-8503A resonance

// ---------------------------------------------------------------------------
// Physical buttons
// ---------------------------------------------------------------------------

inline constexpr gpio_num_t PIN_BUTTON_BOOT = GPIO_NUM_28; // active-low
inline constexpr gpio_num_t PIN_BUTTON_POWER = GPIO_NUM_5; // QON, active-low

// ---------------------------------------------------------------------------
// Capacitive touch (CAP1203, I2C) — interrupt output
// ---------------------------------------------------------------------------

inline constexpr gpio_num_t PIN_CAP_INT = GPIO_NUM_1; // active-low
inline constexpr uint8_t TOUCH_DELTA_SENSE = 0;       // 0-7, 0 = 128x max sensitivity

// ---------------------------------------------------------------------------
// Accelerometer (LIS2DH12, I2C) — INT1 line
//
// INT1 (net ACC_INT) starts awake interrupt-burst shake capture.
// ---------------------------------------------------------------------------

inline constexpr gpio_num_t PIN_ACCEL_INT = GPIO_NUM_3;

// ---------------------------------------------------------------------------
// External watchdog (GPIO pulse)
// ---------------------------------------------------------------------------

inline constexpr gpio_num_t PIN_EXT_WDT = GPIO_NUM_2;

#endif // BOARD_CONFIG_H

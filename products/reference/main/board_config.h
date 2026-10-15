#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/uart.h"

#define BOARD_GO

// Board selection: define exactly one of BOARD_MAX or BOARD_GO before
// including this header (e.g. via -DBOARD_MAX in CMake or Kconfig).
// Defaults to BOARD_MAX if neither is defined.
#if !defined(BOARD_MAX) && !defined(BOARD_GO)
#define BOARD_MAX
#endif

// ---------------------------------------------------------------------------
// BOARD_MAX pin configuration
// ---------------------------------------------------------------------------
#if defined(BOARD_MAX)

// GPIO — board power rails and control lines
inline constexpr gpio_num_t PIN_IO_WDT = GPIO_NUM_2;
inline constexpr gpio_num_t PIN_EN_PMS1 = GPIO_NUM_3;
inline constexpr gpio_num_t PIN_EN_ALPHASENSE = GPIO_NUM_4;
inline constexpr gpio_num_t PIN_EN_CO2 = GPIO_NUM_5;
inline constexpr gpio_num_t PIN_EN_PMS2 = GPIO_NUM_22;
inline constexpr gpio_num_t PIN_EN_CE_CARD = GPIO_NUM_15;
inline constexpr gpio_num_t PIN_IO_CE_POWER = GPIO_NUM_23;

// GPIO — I2C
inline constexpr gpio_num_t PIN_I2C_SCL = GPIO_NUM_6;
inline constexpr gpio_num_t PIN_I2C_SDA = GPIO_NUM_7;

// CAP1203 touch sensitivity. delta_sense: 0–7 (0 = 128x max, 7 = 1x min).
// Set to 0 (maximum) because the board's CS2/CS3 pads require it.
inline constexpr uint8_t TOUCH_DELTA_SENSE = 0;

// GPIO — I2C-UART bridge reset
inline constexpr gpio_num_t PIN_IIC_RESET = GPIO_NUM_21;

// GPIO — UI
inline constexpr gpio_num_t PIN_LED_INDICATOR = GPIO_NUM_10;
inline constexpr gpio_num_t PIN_BOOT_BUTTON = GPIO_NUM_9;

// UART — CO2 sensor
inline constexpr gpio_num_t PIN_UART_RX_CO2 = GPIO_NUM_0;
inline constexpr gpio_num_t PIN_UART_TX_CO2 = GPIO_NUM_1;
inline constexpr uart_port_t UART_PORT_CO2 = UART_NUM_0;
inline constexpr int UART_BAUD_CO2 = 9600;

// SPI bus shared by NAND flash
inline constexpr spi_host_device_t NAND_SPI_HOST = SPI2_HOST;
inline constexpr int NAND_SPI_CLOCK_HZ = 10 * 1000 * 1000;
inline constexpr int NAND_SPI_MAX_TRANSFER_SZ = 4096;

// SPI NAND flash GPIO pins
inline constexpr gpio_num_t PIN_NAND_CS = GPIO_NUM_4;
inline constexpr gpio_num_t PIN_NAND_MOSI = GPIO_NUM_25;
inline constexpr gpio_num_t PIN_NAND_MISO = GPIO_NUM_24;
inline constexpr gpio_num_t PIN_NAND_CLK = GPIO_NUM_23;

// ---------------------------------------------------------------------------
// BOARD_GO pin configuration
// ---------------------------------------------------------------------------
#elif defined(BOARD_GO)

// LIS2DH12 diagnostic: Go INT1 -> GPIO3, external watchdog -> GPIO2.
inline constexpr gpio_num_t PIN_ACCEL_INT = GPIO_NUM_3;
inline constexpr gpio_num_t PIN_ACCEL_TEST_WDT = GPIO_NUM_2;
inline constexpr uint8_t I2C_ADDR_ACCEL_TEST = 0x18;
inline constexpr gpio_num_t PIN_ACCEL_TEST_BUZZER = GPIO_NUM_8;

// GPIO — board power rails and control lines
inline constexpr gpio_num_t PIN_EN_PM = GPIO_NUM_26;

// GPIO — I2C
inline constexpr gpio_num_t PIN_I2C_SCL = GPIO_NUM_6;
inline constexpr gpio_num_t PIN_I2C_SDA = GPIO_NUM_7;

// CAP1203 touch sensitivity
inline constexpr uint8_t TOUCH_DELTA_SENSE = 0;

// GPIO — I2C-UART bridge reset
inline constexpr gpio_num_t PIN_IIC_RESET = GPIO_NUM_21;

// GPIO — UI
inline constexpr gpio_num_t PIN_LED_INDICATOR = GPIO_NUM_10;
inline constexpr gpio_num_t PIN_BOOT_BUTTON = GPIO_NUM_9;

// UART — CO2 sensor
inline constexpr gpio_num_t PIN_UART_RX_CO2 = GPIO_NUM_0;
inline constexpr gpio_num_t PIN_UART_TX_CO2 = GPIO_NUM_1;
inline constexpr uart_port_t UART_PORT_CO2 = UART_NUM_0;
inline constexpr int UART_BAUD_CO2 = 9600;

// SPI bus shared by NAND flash
inline constexpr spi_host_device_t NAND_SPI_HOST = SPI2_HOST;
inline constexpr int NAND_SPI_CLOCK_HZ = 10 * 1000 * 1000;
inline constexpr int NAND_SPI_MAX_TRANSFER_SZ = 4096;

// SPI NAND flash GPIO pins
inline constexpr gpio_num_t PIN_NAND_CS = GPIO_NUM_4;
inline constexpr gpio_num_t PIN_NAND_MOSI = GPIO_NUM_25;
inline constexpr gpio_num_t PIN_NAND_MISO = GPIO_NUM_24;
inline constexpr gpio_num_t PIN_NAND_CLK = GPIO_NUM_23;

#else
#error "Unknown board: define BOARD_MAX or BOARD_GO"
#endif

#endif // BOARD_CONFIG_H

/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AIRGRADIENT_UART_HPP
#define AIRGRADIENT_UART_HPP

#include "hal/uart_types.h"

#include "airgradient_serial.h"

/**
 * Native UART implementation of AirgradientSerial interface.
 * Provides direct ESP-IDF UART communication.
 */
class AirgradientUART : public AirgradientSerial {
public:
  /**
   * Constructor for UART serial communication.
   *
   * @param port_num UART port number (UART_NUM_0, UART_NUM_1, UART_NUM_2)
   * @param rx RX GPIO pin number
   * @param tx TX GPIO pin number
   */
  AirgradientUART(int port_num, int rx, int tx);
  ~AirgradientUART();

  /**
   * Initialize UART with baud rate.
   * Port and pins are configured via constructor.
   *
   * @param baud Baud rate
   * @return true if successful, false otherwise
   */
  bool begin(int baud) override;

  /**
   * Deinitialize UART and free resources.
   */
  void end() override;

  /**
   * Get number of bytes available in UART RX buffer.
   *
   * @return Number of bytes available
   */
  int available() override;

  /**
   * Print null-terminated string to UART.
   *
   * @param str String to print
   */
  void print(const char *str) override;

  /**
   * Write binary data to UART.
   *
   * @param data Pointer to data buffer
   * @param len Length of data in bytes
   * @return Number of bytes written
   */
  int write(const uint8_t *data, int len) override;

  bool wait_tx_done(uint32_t timeout_ms) override;

  /**
   * Read single byte from UART.
   *
   * @return Byte value (0-255), or -1 if no data available
   */
  int read() override;

  /**
   * Bulk read from UART RX FIFO. Non-blocking.
   *
   * @param buf Destination buffer
   * @param len Maximum number of bytes to read
   * @return Number of bytes actually read
   */
  int read(uint8_t *buf, int len) override;

private:
  static constexpr int BUF_SIZE = 1024;
  uart_port_t _port_num;
  int _rx_pin;
  int _tx_pin;
};

#endif // AIRGRADIENT_UART_HPP

/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AIRGRADIENT_SERIAL_HPP
#define AIRGRADIENT_SERIAL_HPP

#include <stddef.h>
#include <stdint.h>

/**
 * Abstract base class for serial communication interfaces.
 * Supports both native UART and I2C-to-UART bridge implementations.
 */
class AirgradientSerial {
public:
  AirgradientSerial();
  virtual ~AirgradientSerial();

  /**
   * Initialize serial communication with specified baud rate.
   *
   * @param baud Baud rate
   * @return true if successful, false otherwise
   */
  virtual bool begin(int baud);

  /**
   * Deinitialize and release serial resources.
   */
  virtual void end();

  /**
   * Get number of bytes available to read.
   *
   * @return Number of bytes in receive buffer
   */
  virtual int available();

  /**
   * Print a null-terminated string.
   *
   * @param str String to print
   */
  virtual void print(const char *str);

  /**
   * Write binary data.
   *
   * @param data Pointer to data buffer
   * @param len Length of data in bytes
   * @return Number of bytes written
   */
  virtual int write(const uint8_t *data, int len);

  /// Wait until all transmitted bits have left the port. Unsupported by default.
  virtual bool wait_tx_done(uint32_t timeout_ms) { return false; }

  /**
   * Read a single byte.
   *
   * @return Byte value (0-255), or -1 if no data available
   */
  virtual int read();

  /**
   * Read up to @p len bytes into @p buf. Non-blocking: returns
   * immediately with whatever is available (0 if nothing). Subclasses
   * may override with a more efficient implementation. The default
   * implementation loops over single-byte read() and is correct but
   * not optimal.
   *
   * @param buf Destination buffer (must not be null)
   * @param len Maximum number of bytes to read
   * @return Number of bytes actually read (0..len)
   */
  virtual int read(uint8_t *buf, int len);

  /**
   * Enable or disable debug output.
   * When enabled, data is echoed to stdout.
   *
   * @param debug true to enable debug output, false to disable
   */
  void setDebug(bool debug);

protected:
  bool isInitialized = false;
  bool isDebug = false;
};

#endif // AIRGRADIENT_SERIAL_HPP

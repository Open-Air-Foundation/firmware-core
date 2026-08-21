/**
 * AirGradient Go — GPS Driver
 *
 * Concrete NMEA GPS driver. Wraps an AirgradientSerial reference for all
 * serial I/O and parses GGA, RMC, and GSA sentences via libnmea-esp32.
 *
 * No virtual methods, no abstract base class. Testable by injecting a
 * StubSerial (byte-queue AirgradientSerial subclass) at construction.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "airgradient_serial.h"
#include "gps/gps_types.h"
#include "gpgga.h"
#include "gpgsa.h"
#include "gprmc.h"
#include "nmea.h"

class GpsDriver {
public:
  /// TAU1113 hardware default baud rate (factory setting, restored on every
  /// power-on reset).
  static constexpr int MODULE_DEFAULT_BAUD = 9600;

  /// Bulk read chunk size used by GpsDriver::read(). Sized to drain the
  /// UART ring buffer in a single call even under worst-case preemption
  /// (~500 ms @ ~960 bytes/sec NMEA data).
  static constexpr size_t READ_CHUNK_SIZE = 512;

  explicit GpsDriver(AirgradientSerial &serial);
  ~GpsDriver();

  /// Initialise the serial link to the GPS module.
  ///
  /// If @p baud_rate differs from MODULE_DEFAULT_BAUD the driver first opens
  /// the UART at 9600 bps, sends the TAU1113 binary command to switch the
  /// module to 115200 bps, tears down the link and re-opens at the target
  /// rate.  If the module was already running at 115200 (e.g. it stayed
  /// powered during deep sleep) the 9600-baud command bytes are received as
  /// garbage and silently ignored, so the sequence is safe in both cases.
  bool begin(int baud_rate);

  /// Reconnect to a receiver that remained powered and tracking while the MCU
  /// was in deep sleep. Opens the UART directly at the active baud rate and
  /// skips receiver configuration so live NMEA output is not drained.
  bool begin_hot_resume(int baud_rate);

  void end();

  /// Process all available serial data and update internal state.
  /// Non-blocking: only consumes bytes currently in the serial buffer.
  /// Returns true if at least one complete NMEA sentence was received.
  bool read();

  /// Inject A-GNSS aiding data into the GPS module via CASIC binary
  /// commands.  Sends AID-POS if the position is valid, AID-TIME if the
  /// time is valid.  Call after begin() and before the read loop.
  ///
  /// If neither position nor time is valid, this is a no-op.
  void inject_aiding(const GpsAidingData &data);

  /// Send TAU1113 CFG-GNSS start. Requires the serial link to be open.
  void gnss_start();

  /// Send TAU1113 CFG-GNSS stop. Requires the serial link to be open.
  void gnss_stop();

  /// Get the latest GPS data snapshot.
  GpsData get_data() const;

  /// Check if the current fix is valid (2D or 3D).
  bool has_valid_fix() const;

private:
  AirgradientSerial &_serial;

  // NMEA sentence accumulation buffer (NMEA 0183 max sentence is 82 chars;
  // 256 bytes provides ample headroom and allows detecting oversized input).
  static constexpr size_t BUFFER_SIZE = 256;
  char _buffer[BUFFER_SIZE];
  size_t _buffer_pos = 0;

  // Current parsed GPS state — initialized to invalid sentinels.
  GpsData _data;

  // Accumulate one byte into the buffer. Returns true when a complete
  // sentence (terminated by \r\n) has been received and dispatched.
  bool _process_byte(char byte);

  // Parse a complete NMEA sentence and update _data. The buffer contents
  // are modified in-place by libnmea (nmea_parse uses _crop_sentence).
  void _handle_sentence(char *sentence, size_t length);

  // Per-sentence-type state updaters.
  void _process_gga(const nmea_gpgga_s *gga);
  void _process_rmc(const nmea_gprmc_s *rmc);
  void _process_gsa(const nmea_gpgsa_s *gsa);

  // Convert a libnmea nmea_position (degrees + decimal minutes + cardinal)
  // to signed decimal degrees. Negative for South / West.
  static double _to_decimal_degrees(const nmea_position &pos);

  // Send the TAU1113 binary command to switch the module UART to 115200 bps.
  void _send_baud_115200_command();

  /// Send AID-POS (0x0B 0x10) with LLA position.
  void _send_aid_pos(double lat_deg, double lon_deg, float alt_m, float pos_acc_m);

  /// Send AID-TIME (0x0B 0x11) with UTC time from epoch.
  void _send_aid_time(int64_t epoch_s, uint32_t time_acc_ms);

  /// Poll MON-VER (0x0A 0x04) and log the module software/hardware version.
  void _poll_mon_ver();

  /// Send CFG-EPHSAVE (0x06 0x10) to enable ephemeris persistence.
  void _send_cfg_ephsave();

  /// Send CFG-NAVSAT (0x06 0x0C) to set constellation enable mask.
  void _send_cfg_navsat();

  /// Poll CFG-NAVSAT (0x06 0x0C) and log the active constellation mask.
  void _poll_cfg_navsat();

  // Return true if the sentence starting at @p buf with length @p len
  // is one of the types GpsDriver consumes (GGA, RMC, GSA). The check
  // assumes a 2-char standard NMEA talker ID at bytes 1..2 and reads
  // the sentence ID at bytes 3..5.
  static bool _is_accepted_sentence(const char *buf, size_t len);
};

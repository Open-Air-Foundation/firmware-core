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
  /// power-on reset and observed after CFG-SLEEP timed wake).
  static constexpr int MODULE_DEFAULT_BAUD = 9600;
  static constexpr int MODULE_ACTIVE_BAUD = 115200;

  /// Bulk read chunk size used by GpsDriver::read(). Sized to drain the
  /// UART ring buffer in a single call even under worst-case preemption
  /// (~500 ms @ ~960 bytes/sec NMEA data).
  static constexpr size_t READ_CHUNK_SIZE = 512;

  explicit GpsDriver(AirgradientSerial &serial);
  ~GpsDriver();

  /// Full acquisition setup: open UART, optionally switch baud, then configure.
  ///
  /// Supports MODULE_DEFAULT_BAUD and MODULE_ACTIVE_BAUD only. Defaults to
  /// 9600; callers explicitly select 115200 for tracking or Always On.
  /// If @p baud_rate is MODULE_ACTIVE_BAUD the driver first opens
  /// the UART at 9600 bps, sends the TAU1113 binary command to switch the
  /// module to 115200 bps, tears down the link and re-opens at the target
  /// rate.  If the module was already running at 115200 (e.g. it stayed
  /// powered during deep sleep) the 9600-baud command bytes are received as
  /// garbage and silently ignored, so the sequence is safe in both cases.
  bool begin(int baud_rate = MODULE_DEFAULT_BAUD);

  /// Open only the host UART at the receiver's current baud rate. Sends no
  /// receiver commands and does not wake GPS. Clears cached fix/parser state.
  /// Call end() before changing baud. Use MODULE_DEFAULT_BAUD after timed wake.
  [[nodiscard]] bool open_uart(int baud_rate);

  /// Poll at the expected baud, then try the other supported baud if silent.
  /// Returns the responding baud, or zero if neither responds. Leaves UART open
  /// on success. Does not change receiver configuration or baud.
  int connect(int expected_baud);

  /// Fast-path sleep probe: MON-VER every 300 ms at 9600 only, for up to 5 s.
  /// Returns 9600 with UART open on response, or zero with UART closed.
  int connect_fast_path();

  /// Send the 115200 command over the currently open UART, drain TX, allow
  /// processing, then reopen the host UART at MODULE_ACTIVE_BAUD. Requires an
  /// awake receiver. No configuration polls or GNSS start command are sent.
  /// False means transport failure; receiver baud may then be unknown.
  [[nodiscard]] bool switch_to_active_baud();

  /// Poll version and configure ephemeris saving/constellations at the current
  /// baud. Requires an awake receiver and open UART. Failures are logged.
  /// Separate from open_uart() so sleep-only cycles need no configuration.
  void configure_receiver();

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

  /// Request CFG-SLEEP deep sleep for a nonzero duration in milliseconds.
  /// Requires an awake receiver and an open UART at its current baud rate.
  /// True means the complete frame finished transmitting, NOT confirmed sleep.
  /// False means invalid duration, incomplete write, or unconfirmed TX drain;
  /// after a TX failure the receiver state is unknown. No automatic retry.
  /// Clears cached data on an attempted request; leaves UART open for end().
  /// No indefinite-sleep sentinel is defined. The caller owns wake scheduling.
  [[nodiscard]] bool request_sleep(uint32_t duration_ms);

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

  /// Send AID-POS (0x0B 0x10) with LLA position.
  void _send_aid_pos(double lat_deg, double lon_deg, float alt_m, float pos_acc_m);

  /// Send AID-TIME (0x0B 0x11) with UTC time from epoch.
  void _send_aid_time(int64_t epoch_s, uint32_t time_acc_ms);

  /// Poll MON-VER (0x0A 0x04) and log the module software/hardware version.
  bool _poll_mon_ver(uint32_t timeout_ms);

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

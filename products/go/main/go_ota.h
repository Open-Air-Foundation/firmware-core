/**
 * AirGradient Go — OTA Service
 *
 * Product-side glue around the airgradient-ota component.  Owns the per-mode
 * OTA wiring: BLE push (Portable) on the borrowed AgBleServer and WiFi pull
 * (Stationary) over a per-call OtaUpdater.  Both transports terminate at the
 * owned EspOtaImageWriter.  The orchestrator drives every entry point on its
 * own task; OtaService never spawns a task and never reboots — it returns an
 * OtaStatus and the orchestrator decides.
 *
 * Foreground / exclusive model: OtaBleService::run() and OtaUpdater::run() are
 * single blocking calls invoked from the orchestrator's OTA poll, after the
 * orchestrator has quiesced every other service.  See products/go/specs/ota.md.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

#include <functional>

#include "backends/esp/esp_ota_image_writer.h"
#include "services/ota_ble_service.h"
#include "types/ota_types.h"

class AgBleServer;
class PowerService;

class OtaService {
public:
  struct Config {
    const char *serial_number = nullptr;    ///< 12-hex device serial
    const char *firmware_version = nullptr; ///< GoBoard::firmware_version()
    const char *http_domain = nullptr;      ///< same AirGradient host as Cloud
    // The WiFi "started downloading" paint does NOT live here — it needs the
    // orchestrator, which is built after the services, so it is passed per-call
    // to run_wifi_check() instead.
  };

  /// Borrows the shared BLE server and the PowerService (for the external
  /// watchdog); owns the writer.  Both must outlive this service.
  OtaService(AgBleServer &server, PowerService &power, const Config &cfg);

  OtaService(const OtaService &) = delete;
  OtaService &operator=(const OtaService &) = delete;

  /// Register the OTA GATT service on the borrowed server.  MUST run between
  /// the BLE server's register phase and start_advertising() (Portable only).
  bool setup_ble();

  /// Forward the borrowed server's disconnect so an in-flight transfer aborts.
  /// Invoked synchronously from BleService::on_disconnect() via the disconnect
  /// observer (NimBLE host-task context).
  void handle_disconnect();

  /// Non-blocking start-edge probe for the orchestrator's periodic poll.  Thin
  /// wrapper over OtaBleService::is_active(): true the moment a valid START
  /// latches (before begin()) and cleared at the terminal.
  bool is_ble_active() const;

  /// Drive a phone-initiated BLE transfer to its terminal (wraps run(0); the
  /// START is already latched so run() drives immediately).  Blocks on the
  /// caller (orchestrator) task.  Returns the result.
  /// Pass false to reject a pending START before preparing or writing the image.
  OtaStatus run_ble(bool start_allowed = true);

  /// Lightweight availability probe. Called by the cloud worker; never
  /// touches the writer, display, or watchdog. Ok means an image is offered.
  OtaStatus check_wifi_update() const;

  /// Run one device-initiated WiFi availability check + download.  Blocks on
  /// the caller task.  Returns UpToDate when no update.  on_download_started is
  /// invoked exactly once, on the first Downloading tick (only when an image is
  /// really being pulled), so the orchestrator can lazily paint "Updating
  /// firmware…".  May be empty in tests.
  OtaStatus run_wifi_check(const std::function<void()> &on_download_started);

  /// Abort any in-flight transfer and clear the OTA GATT registration.
  /// Idempotent.  Never deinit()s the server.  Portable-only.
  void teardown_ble();

private:
  /// Named BLE progress handler.  Feeds the external watchdog on every
  /// non-terminal tick (Starting/Downloading/Applying).
  void _on_ble_progress(const OtaProgress &progress);

  /// Named WiFi pull progress handler.  Feeds the external watchdog on every
  /// non-terminal tick and fires the stored _on_download_started once on the
  /// first Downloading tick (one-shot guarded by _wifi_download_painted).
  void _on_wifi_progress(const OtaProgress &progress);

  AgBleServer &_server;
  PowerService &_power; ///< external-watchdog feeder; alive before OtaService
  Config _config;
  EspOtaImageWriter _writer;
  OtaBleService _ble_ota; ///< constructed over (_server, _writer)

  // Per-call commit notification, stored for the duration of run_wifi_check()
  // so the named _on_wifi_progress() member can invoke it.
  std::function<void()> _on_download_started;
  bool _wifi_download_painted = false; ///< one-shot guard; reset per run_wifi_check()
};

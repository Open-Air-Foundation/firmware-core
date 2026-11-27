/**
 * AirGradient Go — OTA Service implementation
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_ota.h"

#include "ag_log.h"
#include "go_power.h"
#include "hal/ble_server.h"
#include "services/ota_updater.h"
#include "backends/wifi/wifi_http_ota_source.h"

static constexpr const char *TAG = "OtaService";

OtaService::OtaService(AgBleServer &server, PowerService &power, const Config &cfg)
    : _server(server), _power(power), _config(cfg), _writer(), _ble_ota(_server, _writer) {
  // Thin forwarder; logic lives in the named handler.
  _ble_ota.set_on_progress([this](const OtaProgress &p) { _on_ble_progress(p); });
}

bool OtaService::setup_ble() { return _ble_ota.setup(); }

void OtaService::handle_disconnect() { _ble_ota.handle_disconnect(); }

bool OtaService::is_ble_active() const { return _ble_ota.is_active(); }

OtaStatus OtaService::run_ble(bool start_allowed) {
  // START is already latched (is_ble_active() returned true), so run(0) drives
  // the transfer immediately with no idle park.
  return _ble_ota.run(0, start_allowed);
}

OtaStatus OtaService::check_wifi_update() const {
  const OtaRequest request{_config.serial_number, _config.firmware_version, _config.http_domain};
  WifiHttpOtaSource source(request);
  return source.check_available();
}

OtaStatus OtaService::run_wifi_check(const std::function<void()> &on_download_started) {
  _wifi_download_painted = false;             // re-arm the one-shot lazy paint
  _on_download_started = on_download_started; // store for the named handler

  OtaRequest request{_config.serial_number, _config.firmware_version, _config.http_domain};
  WifiHttpOtaSource source(request);
  OtaUpdater updater(source, _writer);
  updater.set_on_progress([this](const OtaProgress &p) { _on_wifi_progress(p); }); // thin forwarder

  const OtaStatus status = updater.run(); // single blocking call
  _on_download_started = nullptr;         // notification valid only during the call
  return status;
}

void OtaService::teardown_ble() { _ble_ota.teardown(); }

void OtaService::_on_ble_progress(const OtaProgress &progress) {
  // Feed the external watchdog on non-terminal ticks (START detection is the
  // orchestrator's is_ble_active() poll, not here).
  const bool terminal = progress.state == OtaState::Done || progress.state == OtaState::Failed ||
                        progress.state == OtaState::Skipped;
  if (!terminal) {
    _power.reset_ext_watchdog();
  }
}

void OtaService::_on_wifi_progress(const OtaProgress &progress) {
  const bool terminal = progress.state == OtaState::Done || progress.state == OtaState::Failed ||
                        progress.state == OtaState::Skipped;
  if (!terminal) {
    _power.reset_ext_watchdog(); // non-terminal ticks only
  }

  // Lazy commit: only once an image is actually being pulled.
  if (progress.state == OtaState::Downloading && !_wifi_download_painted) {
    _wifi_download_painted = true;
    if (_on_download_started) {
      _on_download_started(); // orchestrator: enter_ota() + paint
    }
  }
}

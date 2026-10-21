/**
 * AirGradient Go — PortableWifiProvisioner implementation
 *
 * Attached BLE-only Wi-Fi provisioning over the bonded Portable link.
 * Mechanics only; policy and persistence live in the orchestrator.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_portable_provisioner.h"

#include "ag_log.h"
#include "common.h"
#include "go_board.h"
#include "go_events.h"
#include "services/provisioning_manager.h"
#include "services/wifi_manager.h"
#include "types/wifi_types.h"

static constexpr const char *TAG = "PortableProv";

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

PortableWifiProvisioner::PortableWifiProvisioner(RtosQueueHandle event_queue, const Deps &deps,
                                                 const Config &cfg)
    : _event_queue(event_queue), _wifi(deps.wifi), _ble(deps.ble), _board(deps.board), _cfg(cfg) {}

PortableWifiProvisioner::~PortableWifiProvisioner() {
  stop();
  delete _manager;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool PortableWifiProvisioner::attach() {
  if (_attached) {
    return true;
  }
  log_heap(TAG, "portable-prov.attach:enter");

  _ensure_provisioning_manager();

  ProvisioningConfig config{};
  config.transport = ProvisioningTransport::BleAttached;
  config.ble.device_name = _cfg.ble_device_name;
  config.ble.model_name = _cfg.ble_model_name;
  config.ble.serial_number = _cfg.ble_serial_number;
  config.ble.firmware_version = _cfg.ble_firmware_version;
  // Server already secured by BleService; io_cap/auth unused here. Component
  // timeout off — the radio is bounded by the product-side idle timer.
  config.overall_timeout_ms = 0;

  if (!_manager->start_attached(_wifi, _ble, config)) {
    AG_LOGE(TAG, "start_attached failed");
    return false;
  }
  _attached = true;
  AG_LOGI(TAG, "attached (provisioning service registered, radio off)");
  log_heap(TAG, "portable-prov.attach:exit");
  return true;
}

void PortableWifiProvisioner::stop() {
  if (!_attached) {
    return;
  }
  AG_LOGI(TAG, "stop");
  // Clears callbacks + detaches the server (no deinit, no mode change).
  _manager->stop();
  // Drop after callbacks are cleared so set_mode(Off) can't re-enter the manager.
  _drop_radio();
  _attached = false;

  _pending_mutex.lock();
  _pending_valid = false;
  _pending_mutex.unlock();
}

// ---------------------------------------------------------------------------
// Orchestrator-task request driver
// ---------------------------------------------------------------------------

void PortableWifiProvisioner::handle_pending_request() {
  AttachedRequest req;
  _pending_mutex.lock();
  if (!_pending_valid) {
    _pending_mutex.unlock();
    return;
  }
  req = _pending_request;
  _pending_valid = false;
  _pending_mutex.unlock();

  if (!_attached) {
    return; // session torn down between post and drain
  }

  if (!_ensure_wifi_ready()) {
    AG_LOGW(TAG, "ensure_wifi_ready failed — rejecting request kind=%u",
            static_cast<unsigned>(req.kind));
    // Surface a failure so the app doesn't hang; scan rejection is benign.
    if (req.kind == AttachedRequestKind::Credentials) {
      _manager->send_ble_status(ProvisioningBleStatus::WIFI_CONNECT_FAILED);
    }
    return;
  }

  _arm_idle_timer();

  if (req.kind == AttachedRequestKind::Scan) {
    _manager->request_scan();
  } else {
    _manager->submit_credentials(req.data);
  }
}

// ---------------------------------------------------------------------------
// Result routing
// ---------------------------------------------------------------------------

void PortableWifiProvisioner::on_connected() {
  AG_LOGI(TAG, "connected — verify-then-drop");
  _drop_radio();
  _manager->reset_to_listening();
}

void PortableWifiProvisioner::on_ble_disconnected() {
  AG_LOGI(TAG, "ble disconnected — dropping radio");
  _drop_radio();
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

bool PortableWifiProvisioner::is_radio_active() const { return _radio_active.load(); }

bool PortableWifiProvisioner::is_attached() const { return _attached; }

// ---------------------------------------------------------------------------
// Timer integration
// ---------------------------------------------------------------------------

uint32_t PortableWifiProvisioner::next_deadline_ms() const { return _radio_idle_deadline_ms; }

void PortableWifiProvisioner::tick(uint32_t now_ms) {
  if (_radio_idle_deadline_ms != 0 && now_ms >= _radio_idle_deadline_ms) {
    AG_LOGI(TAG, "radio-idle timeout — dropping radio");
    _drop_radio(); // also cancels the deadline
  }
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

void PortableWifiProvisioner::_ensure_provisioning_manager() {
  if (_manager != nullptr) {
    return;
  }

  _manager = new ProvisioningManager();
  _manager->set_on_event(
      [this](const ProvisioningEventInfo &info) { _on_provisioning_event(info); });
  _manager->set_attached_request_hook(
      [this](const AttachedRequest &req) { _on_attached_request(req); });
}

void PortableWifiProvisioner::_on_attached_request(const AttachedRequest &req) {
  // NimBLE task: buffer the request and signal the orchestrator (no Wi-Fi work).
  _pending_mutex.lock();
  _pending_request = req;
  _pending_valid = true;
  _pending_mutex.unlock();

  Event evt{};
  evt.type = EventType::PortableProvRequest;
  RTOS::queue_send(_event_queue, &evt);
}

void PortableWifiProvisioner::_on_provisioning_event(const ProvisioningEventInfo &info) {
  // Tagged BleAttached so the orchestrator routes it to the silent Portable path.
  Event evt{};
  evt.type = EventType::ProvisioningStateChanged;
  evt.prov.event = static_cast<uint8_t>(info.event);
  evt.prov.transport = static_cast<uint8_t>(ProvisioningTransport::BleAttached);
  evt.prov.stop_reason = static_cast<uint8_t>(info.stop_reason);
  evt.prov.ip = info.ip;
  evt.prov.disable_cloud = info.data.disable_cloud;
  evt.prov.static_ip = info.data.static_ip;
  RTOS::queue_send(_event_queue, &evt);
}

bool PortableWifiProvisioner::_ensure_wifi_ready() {
  // Lazy, idempotent Wi-Fi heap init on the first request.
  if (!_wifi_initialized) {
    _board.init_wifi_subsystem();
    _wifi_initialized = true;
  }
  // STA only — the only BLE-coexistence-safe mode on this target.
  if (_wifi.set_mode(WifiMode::Sta) != WifiStatus::Ok) {
    AG_LOGE(TAG, "set_mode(Sta) failed");
    return false;
  }
  // The shared radio may retain Stationary's power-saving preference.
  const WifiStatus status = _wifi.set_power_save(WifiPowerSave::None);
  if (status != WifiStatus::Ok) {
    AG_LOGW(TAG, "failed to disable provisioning Wi-Fi power save: %u",
            static_cast<unsigned>(status));
  }
  _radio_active.store(true);
  return true;
}

void PortableWifiProvisioner::_drop_radio() {
  _cancel_idle_timer();
  if (_radio_active.load()) {
    _wifi.set_mode(WifiMode::Off);
    _radio_active.store(false);
  }
}

void PortableWifiProvisioner::_arm_idle_timer() {
  _radio_idle_deadline_ms = static_cast<uint32_t>(RTOS::get_time_ms()) + _cfg.radio_idle_ms;
}

void PortableWifiProvisioner::_cancel_idle_timer() { _radio_idle_deadline_ms = 0; }

#ifndef GO_EVENTS_H
#define GO_EVENTS_H

#include <cstdint>

#include "go_cloud_types.h"
#include "go_types.h"
#include "gps/gps_types.h"
#include "measures_types.h"
#include "go_wifi_types.h"

// --- Event type discriminator ---

enum class EventType : uint8_t {
  // --- Producer events ---
  SensorDataReady, // payload: MeasuresAGo
  CalSampleReady,  // payload: ThermalSample cal_sample (1 Hz cal-stream tick)
  PmSensorAsleep,  // no payload (PM sleep done; orchestrator may isolate now)
  SensorTestDone,  // payload: SensorTestResults sensor_test_results (bulk AQ self-test)
  GpsFixUpdate,    // payload: GpsData
  InputPress,      // payload: InputEventData

  // --- System events ---
  InactivityTimeout, // no payload
  MeasurementTimer,  // no payload
  WakeFromSleep,     // payload: WakeEventData

  // --- BLE events ---
  BleConnected,      // no payload
  BleDisconnected,   // no payload
  BleConfigWrite,    // no payload (data in BleService pending buffer)
  BleHistoryWrite,   // no payload (data in BleService pending buffer)
  BlePairingRequest, // payload: uint32_t ble_passkey
  BleAuthComplete,   // payload: bool ble_auth_ok (link encrypted/authenticated)

  // --- Wi-Fi events ---
  WifiConnected,            // payload: uint32_t wifi_ip (network byte order)
  WifiDisconnected,         // payload: uint8_t wifi_disconnect_reason
  ProvisioningStateChanged, // payload: ProvisioningEventPayload prov
  PortableProvRequest,      // no payload (parsed request in provisioner buffer)

  // --- Cloud transport events ---
  PostMeasuresResult, // payload: uint8_t cloud_result (AgClientResult)
  FetchConfigResult,  // payload: uint8_t cloud_result (AgClientResult)

  // --- Calibration events ---
  Co2CalibrationDone, // payload: uint8_t co2_cal_result (Co2CalibrationResult)

  // --- UI action events ---
  UserStartTracking, // no payload
  UserStopTracking,  // no payload
  UserChangeMode,    // payload: OperatingMode
  UserToggleGps,     // payload: bool
  SettingsChanged,   // no payload
  ClearData,         // no payload
  SaveTag,           // payload: tag_index (uint8_t)
};

// --- Event payload structs ---

struct InputEventData {
  InputSource source;
  InputType type;
};

/// Result of the bulk AQ hardware self-test (Peripheral Test). One bool per
/// Go sensor role: true = a field-valid reading was obtained, false = FAIL
/// (absent or bad reading — the two collapse into a single failure).
struct SensorTestResults {
  bool co2_pass = false;
  bool pm_pass = false;
  bool temp_hum_pass = false;
  bool tvoc_nox_pass = false;
  bool pressure_pass = false;

  bool all_pass() const {
    return co2_pass && pm_pass && temp_hum_pass && tvoc_nox_pass && pressure_pass;
  }
};

struct WakeEventData {
  WakeCause cause;
};

// --- Event struct ---
//
// Fixed-size struct with a type discriminator and a union of all possible
// payloads. Union size is dominated by GpsData (~68 bytes). Events with no
// payload only use the type field; the union is allocated but not accessed.
//
// Note: always initialize with braces (e.g. Event evt{}) rather than plain
// declaration, because GpsData default member initializers make the anonymous
// union's default constructor deleted.

struct Event {
  EventType type;

  union {
    MeasuresAGo sensor_data;               // SensorDataReady
    ThermalSample cal_sample;              // CalSampleReady
    GpsData gps_data;                      // GpsFixUpdate (~68 bytes)
    InputEventData input;                  // InputPress (2 bytes)
    OperatingMode mode_change;             // UserChangeMode (1 byte)
    WakeEventData wake;                    // WakeFromSleep (1 byte)
    bool gps_enabled;                      // UserToggleGps (1 byte)
    bool ble_auth_ok;                      // BleAuthComplete (1 byte, link encrypted)
    uint8_t tag_index;                     // SaveTag (1 byte)
    uint32_t ble_passkey;                  // BlePairingRequest (4 bytes)
    uint8_t co2_cal_result;                // Co2CalibrationDone (1 byte, Co2CalibrationResult)
    SensorTestResults sensor_test_results; // SensorTestDone (5 bools)
    uint32_t wifi_ip;                      // WifiConnected (network byte order)
    uint8_t wifi_disconnect_reason;        // WifiDisconnected (WifiDisconnectReason)
    ProvisioningEventPayload prov;         // ProvisioningStateChanged
    CloudResultByte cloud_result; // PostMeasuresResult / FetchConfigResult (AgClientResult byte)
  };
};

// Queue memory footprint: EVENT_QUEUE_DEPTH * sizeof(Event) ~ 2.6 KB
static constexpr uint8_t EVENT_QUEUE_DEPTH = 16;

#endif // GO_EVENTS_H

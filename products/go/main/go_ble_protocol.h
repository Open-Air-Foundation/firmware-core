/**
 * AirGradient Go — BLE Protocol String Constants
 *
 * CBOR key names, type discriminators, operation values, command strings,
 * error strings, and enum-to-wire mappings used by the BLE GATT service.
 * Shared between go_ble.cpp (encoding/decoding) and go_orchestrator.cpp
 * (command result error strings).
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

// ---------------------------------------------------------------------------
// Common protocol keys (shared across characteristics)
// ---------------------------------------------------------------------------

inline constexpr const char *BLE_KEY_TYPE = "type";
inline constexpr const char *BLE_KEY_OP = "op";
inline constexpr const char *BLE_KEY_CMD = "cmd";
inline constexpr const char *BLE_KEY_OK = "ok";
inline constexpr const char *BLE_KEY_ERR = "err";
inline constexpr const char *BLE_KEY_SESSION = "session";
inline constexpr const char *BLE_KEY_SENT = "sent";

// ---------------------------------------------------------------------------
// Measures characteristic keys
// ---------------------------------------------------------------------------

inline constexpr const char *BLE_KEY_TEMP = "t";
inline constexpr const char *BLE_KEY_HUM = "h";
inline constexpr const char *BLE_KEY_PM1 = "pm1";
inline constexpr const char *BLE_KEY_PM25 = "pm25";
inline constexpr const char *BLE_KEY_PM10 = "pm10";
inline constexpr const char *BLE_KEY_CO2 = "co2";
inline constexpr const char *BLE_KEY_TVOC = "tvoc";
inline constexpr const char *BLE_KEY_NOX = "nox";
inline constexpr const char *BLE_KEY_PRES = "pres";
inline constexpr const char *BLE_KEY_LAT = "lat";
inline constexpr const char *BLE_KEY_LON = "lon";
inline constexpr const char *BLE_KEY_ALT = "alt";
inline constexpr const char *BLE_KEY_FIX = "fix";
inline constexpr const char *BLE_KEY_SAT = "sat";
inline constexpr const char *BLE_KEY_TS = "ts";

// ---------------------------------------------------------------------------
// Status characteristic keys
// ---------------------------------------------------------------------------

inline constexpr const char *BLE_KEY_GPS_FIX = "gps_fix";
inline constexpr const char *BLE_KEY_GPS_SAT = "gps_sat";
inline constexpr const char *BLE_KEY_BAT_PCT = "bat_pct";
inline constexpr const char *BLE_KEY_BAT_V = "bat_v";
inline constexpr const char *BLE_KEY_CHARGING = "charging";
inline constexpr const char *BLE_KEY_TRACKING = "tracking";
inline constexpr const char *BLE_KEY_FLASH_KB = "flash_kb";
inline constexpr const char *BLE_KEY_USED_KB = "used_kb";
inline constexpr const char *BLE_KEY_DISC = "disc";

// ---------------------------------------------------------------------------
// Cal characteristic keys (thermal-calibration telemetry)
// Reuses BLE_KEY_TEMP ("t"), BLE_KEY_HUM ("h"), BLE_KEY_PRES ("pres"),
// BLE_KEY_TS ("ts") from the Measures keys, and BLE_VAL_CHARGE_* strings.
// ---------------------------------------------------------------------------

inline constexpr const char *BLE_KEY_CAL_TDPS = "tdps"; // DPS368 temp (°C, on main PCB)
inline constexpr const char *BLE_KEY_CAL_TFG = "tfg";   // fuel-gauge internal temp (°C)
inline constexpr const char *BLE_KEY_CAL_TDIE = "tdie"; // BQ25629 die temp (°C)
inline constexpr const char *BLE_KEY_CAL_TBAT = "tbat"; // battery NTC temp (°C)
inline constexpr const char *BLE_KEY_CAL_IBAT = "ibat"; // FG battery current (mA, +charge)
inline constexpr const char *BLE_KEY_CAL_ICHG = "ichg"; // charger battery current (mA)
inline constexpr const char *BLE_KEY_CAL_IBUS = "ibus"; // input/VBUS current (mA)
inline constexpr const char *BLE_KEY_CAL_VBUS = "vbus"; // VBUS voltage (V)
inline constexpr const char *BLE_KEY_CAL_VBAT = "vbat"; // battery voltage (V)
inline constexpr const char *BLE_KEY_CAL_CHG = "chg";   // charging state string
inline constexpr const char *BLE_KEY_CAL_GPS = "gps";   // GPS receiver active (0/1)
inline constexpr const char *BLE_KEY_CAL_UP = "up";     // uptime (s)

// ---------------------------------------------------------------------------
// Config characteristic keys
// ---------------------------------------------------------------------------

inline constexpr const char *BLE_KEY_MEAS_INT = "meas_int";
inline constexpr const char *BLE_KEY_PM_INT = "pm_int";
inline constexpr const char *BLE_KEY_OTHER_INT = "other_int";
inline constexpr const char *BLE_KEY_DISP_INT = "disp_int";
inline constexpr const char *BLE_KEY_TEMP_F = "temp_f";
inline constexpr const char *BLE_KEY_PM_AQI = "pm_aqi";
inline constexpr const char *BLE_KEY_GPS_INT = "gps_int";
inline constexpr const char *BLE_KEY_GPS_MODE = "gps_mode";
inline constexpr const char *BLE_KEY_INACT_TO = "inact_to";
inline constexpr const char *BLE_KEY_AUTO_LOCK = "auto_lock";
inline constexpr const char *BLE_KEY_DEV_NAME = "dev_name";
inline constexpr const char *BLE_KEY_OP_MODE = "op_mode";
inline constexpr const char *BLE_KEY_FRONT_LED = "fled";
inline constexpr const char *BLE_KEY_BACK_LED = "bled";
inline constexpr const char *BLE_KEY_TOUCH_LED = "tled";

// ---------------------------------------------------------------------------
// Aiding command keys (payload fields for "set_aiding" command)
// ---------------------------------------------------------------------------

inline constexpr const char *BLE_KEY_POS_ACC = "pos_acc";
inline constexpr const char *BLE_KEY_EPOCH = "epoch";
inline constexpr const char *BLE_KEY_TIME_ACC = "time_acc";
// Also uses BLE_KEY_LAT, BLE_KEY_LON, BLE_KEY_ALT from Measures keys

// ---------------------------------------------------------------------------
// History characteristic keys
// ---------------------------------------------------------------------------

inline constexpr const char *BLE_KEY_SESSIONS = "sessions";
inline constexpr const char *BLE_KEY_ID = "id";
inline constexpr const char *BLE_KEY_PTS = "pts";
inline constexpr const char *BLE_KEY_TOTAL = "total";
inline constexpr const char *BLE_KEY_PT_SIZE = "pt_size";
inline constexpr const char *BLE_KEY_PG = "pg";
inline constexpr const char *BLE_KEY_TPG = "tpg";
inline constexpr const char *BLE_KEY_CNT = "cnt";

// ---------------------------------------------------------------------------
// Type discriminator values (value of BLE_KEY_TYPE)
// ---------------------------------------------------------------------------

inline constexpr const char *BLE_VAL_TYPE_CONFIG = "config";
inline constexpr const char *BLE_VAL_TYPE_CMD_RESULT = "cmd_result";
inline constexpr const char *BLE_VAL_TYPE_CMD_PROGRESS = "cmd_progress";
inline constexpr const char *BLE_VAL_TYPE_SESSIONS = "sessions";
inline constexpr const char *BLE_VAL_TYPE_STARTED = "started";
inline constexpr const char *BLE_VAL_TYPE_DONE = "done";
inline constexpr const char *BLE_VAL_TYPE_ENDED = "ended";
inline constexpr const char *BLE_VAL_TYPE_DELETED = "deleted";
inline constexpr const char *BLE_VAL_TYPE_ERROR = "error";

// ---------------------------------------------------------------------------
// Disconnect-notice values (value of BLE_KEY_DISC)
// ---------------------------------------------------------------------------

inline constexpr const char *BLE_VAL_DISC_OVERHEAT = "overheat";           // OT shutdown
inline constexpr const char *BLE_VAL_DISC_LOW_BATT = "low_batt";           // EDV shutdown
inline constexpr const char *BLE_VAL_DISC_USER = "user";                   // user long-press
inline constexpr const char *BLE_VAL_DISC_OP_STATIONARY = "op_stationary"; // mode -> Stationary
inline constexpr const char *BLE_VAL_DISC_OP_OFFLINE = "op_offline";       // mode -> Offline

// ---------------------------------------------------------------------------
// Operation values (value of BLE_KEY_OP)
// ---------------------------------------------------------------------------

inline constexpr const char *BLE_VAL_OP_SET = "set";
inline constexpr const char *BLE_VAL_OP_CMD = "cmd";
inline constexpr const char *BLE_VAL_OP_LIST = "list";
inline constexpr const char *BLE_VAL_OP_START = "start";
inline constexpr const char *BLE_VAL_OP_FILL = "fill";
inline constexpr const char *BLE_VAL_OP_END = "end";
inline constexpr const char *BLE_VAL_OP_DELETE = "delete";

// ---------------------------------------------------------------------------
// Error string values (value of BLE_KEY_ERR)
// ---------------------------------------------------------------------------

// History errors
inline constexpr const char *BLE_VAL_ERR_SESSION_NOT_FOUND = "session_not_found";
inline constexpr const char *BLE_VAL_ERR_FLASH_ERROR = "flash_error";
inline constexpr const char *BLE_VAL_ERR_NO_ACTIVE_DOWNLOAD = "no_active_download";
inline constexpr const char *BLE_VAL_ERR_DELETE_FAILED = "delete_failed";
inline constexpr const char *BLE_VAL_ERR_SESSION_ACTIVE = "session_active";
// History export rejected while the provisioning radio is active.
inline constexpr const char *BLE_VAL_ERR_BUSY = "busy";

// Command result errors
inline constexpr const char *BLE_VAL_ERR_UNSUPPORTED = "unsupported";
inline constexpr const char *BLE_VAL_ERR_CALIBRATION_FAILED = "calibration_failed";
inline constexpr const char *BLE_VAL_ERR_CLEAR_FAILED = "clear_failed";
inline constexpr const char *BLE_VAL_ERR_FACTORY_RESET_FAILED = "factory_reset_failed";
inline constexpr const char *BLE_VAL_ERR_ALREADY_TRACKING = "already_tracking";
inline constexpr const char *BLE_VAL_ERR_NOT_TRACKING = "not_tracking";
inline constexpr const char *BLE_VAL_ERR_UNKNOWN_COMMAND = "unknown_command";
inline constexpr const char *BLE_VAL_ERR_UNKNOWN_CONFIG_KEY = "unknown_config_key";
inline constexpr const char *BLE_VAL_ERR_NO_AIDING_DATA = "no_aiding_data";
// A "set" carrying more than one recognized config key is rejected: NOTIFY
// deltas are bounded to a single field per event.
inline constexpr const char *BLE_VAL_ERR_SINGLE_FIELD_ONLY = "single_field_only";

// ---------------------------------------------------------------------------
// GPS mode string values
// ---------------------------------------------------------------------------

inline constexpr const char *BLE_VAL_GPS_OFF = "off";
inline constexpr const char *BLE_VAL_GPS_TRACKING = "tracking";
inline constexpr const char *BLE_VAL_GPS_ALWAYS = "always";

// ---------------------------------------------------------------------------
// Operating mode string values
// ---------------------------------------------------------------------------

inline constexpr const char *BLE_VAL_MODE_PORTABLE = "portable";
inline constexpr const char *BLE_VAL_MODE_STATIONARY = "stationary";
inline constexpr const char *BLE_VAL_MODE_OFFLINE = "offline";

// ---------------------------------------------------------------------------
// BLE command string values
// ---------------------------------------------------------------------------

inline constexpr const char *BLE_VAL_CMD_CO2_CAL = "co2_cal";
inline constexpr const char *BLE_VAL_CMD_CLEAR_DATA = "clear_data";
inline constexpr const char *BLE_VAL_CMD_FACTORY_RST = "factory_rst";
inline constexpr const char *BLE_VAL_CMD_START_TRACKING = "start_tracking";
inline constexpr const char *BLE_VAL_CMD_STOP_TRACKING = "stop_tracking";
inline constexpr const char *BLE_VAL_CMD_SET_AIDING = "set_aiding";
inline constexpr const char *BLE_VAL_CMD_SET = "set";
inline constexpr const char *BLE_VAL_CMD_UNKNOWN = "unknown";

// ---------------------------------------------------------------------------
// Charging state string values
// ---------------------------------------------------------------------------

inline constexpr const char *BLE_VAL_CHARGE_NONE = "none";
inline constexpr const char *BLE_VAL_CHARGE_TRICKLE = "trickle";
inline constexpr const char *BLE_VAL_CHARGE_PRE = "pre";
inline constexpr const char *BLE_VAL_CHARGE_FAST = "fast";
inline constexpr const char *BLE_VAL_CHARGE_TAPER = "taper";
inline constexpr const char *BLE_VAL_CHARGE_TOPOFF = "topoff";
inline constexpr const char *BLE_VAL_CHARGE_DONE = "done";
inline constexpr const char *BLE_VAL_CHARGE_UNKNOWN = "unknown";

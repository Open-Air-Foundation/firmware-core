/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_settings.h"

#include <map>
#include <string>
#include <variant>

#include <catch2/catch_test_macros.hpp>

// ============================================================================
// FakeConfigStore — in-memory ConfigStore for round-trip testing
// ============================================================================

class FakeConfigStore : public ConfigStore {
public:
  ConfigStoreResult get_int(const char *key, int &out) override {
    auto it = _ints.find(key);
    if (it == _ints.end()) {
      return ConfigStoreResult::NOT_FOUND;
    }
    out = it->second;
    return ConfigStoreResult::OK;
  }

  ConfigStoreResult set_int(const char *key, int value) override {
    if (_fail_writes) {
      return ConfigStoreResult::ERROR;
    }
    _ints[key] = value;
    return ConfigStoreResult::OK;
  }

  ConfigStoreResult get_bool(const char *key, bool &out) override {
    auto it = _bools.find(key);
    if (it == _bools.end()) {
      return ConfigStoreResult::NOT_FOUND;
    }
    out = it->second;
    return ConfigStoreResult::OK;
  }

  ConfigStoreResult set_bool(const char *key, bool value) override {
    if (_fail_writes) {
      return ConfigStoreResult::ERROR;
    }
    _bools[key] = value;
    return ConfigStoreResult::OK;
  }

  ConfigStoreResult get_string(const char *key, std::string &out) override {
    auto it = _strings.find(key);
    if (it == _strings.end()) {
      return ConfigStoreResult::NOT_FOUND;
    }
    out = it->second;
    return ConfigStoreResult::OK;
  }

  ConfigStoreResult set_string(const char *key, const std::string &value) override {
    if (_fail_writes) {
      return ConfigStoreResult::ERROR;
    }
    _strings[key] = value;
    return ConfigStoreResult::OK;
  }

  ConfigStoreResult erase(const char *key) override {
    _ints.erase(key);
    _bools.erase(key);
    _strings.erase(key);
    return ConfigStoreResult::OK;
  }

  ConfigStoreResult commit() override {
    if (_fail_writes) {
      return ConfigStoreResult::ERROR;
    }
    _committed = true;
    return ConfigStoreResult::OK;
  }

  bool committed() const { return _committed; }
  void reset_committed() { _committed = false; }
  void set_fail_writes(bool fail) { _fail_writes = fail; }
  bool has_int(const char *key) const { return _ints.count(key) > 0; }

private:
  std::map<std::string, int> _ints;
  std::map<std::string, bool> _bools;
  std::map<std::string, std::string> _strings;
  bool _committed = false;
  bool _fail_writes = false;
};

// ============================================================================
// Defaults — load from empty store returns struct defaults
// ============================================================================

TEST_CASE("load from empty store returns struct defaults", "[settings]") {
  FakeConfigStore store;
  GoSettings s = load_go_settings(store);

  REQUIRE(s.measure_interval_seconds == 10);
  REQUIRE(s.inactivity_timeout_seconds == 5);
  REQUIRE(s.gps_interval_seconds == 5);
  REQUIRE(s.gps_mode == GpsMode::OnWhenTracking);
  REQUIRE(s.operating_mode == OperatingMode::Portable);
  REQUIRE(s.device_name == "airgradient-go");
  REQUIRE(s.use_fahrenheit == false);
  REQUIRE(s.pm_use_usaqi == false);
  REQUIRE(s.auto_lock_seconds == 10);
  REQUIRE(s.disable_cloud == false);
  REQUIRE(s.static_ip.ip == 0);
  REQUIRE(s.static_ip.netmask == 0);
  REQUIRE(s.static_ip.gateway == 0);
  REQUIRE(s.static_ip.dns_primary == 0);
  REQUIRE(s.static_ip.dns_secondary == 0);
  REQUIRE(s.onboarding_done == false);
}

// ============================================================================
// First-boot onboarding flag — absent key defaults false; round-trips
// ============================================================================

TEST_CASE("onboarding_done absent key loads as false", "[settings][onboarding]") {
  FakeConfigStore store;
  REQUIRE(load_go_settings(store).onboarding_done == false);
}

TEST_CASE("onboarding_done round-trips true", "[settings][onboarding]") {
  FakeConfigStore store;

  GoSettings original;
  original.onboarding_done = true;
  REQUIRE(save_go_settings(store, original));

  REQUIRE(load_go_settings(store).onboarding_done == true);
}

TEST_CASE("onboarding_done round-trips false", "[settings][onboarding]") {
  FakeConfigStore store;

  // Persist true first, then re-save false to confirm it overwrites.
  GoSettings on;
  on.onboarding_done = true;
  REQUIRE(save_go_settings(store, on));

  GoSettings off;
  off.onboarding_done = false;
  REQUIRE(save_go_settings(store, off));

  REQUIRE(load_go_settings(store).onboarding_done == false);
}

// ============================================================================
// Round-trip — save then load preserves every field
// ============================================================================

TEST_CASE("save then load round-trips all fields", "[settings]") {
  FakeConfigStore store;

  GoSettings original;
  original.measure_interval_seconds = 60;
  original.inactivity_timeout_seconds = 30;
  original.gps_interval_seconds = 10;
  original.gps_mode = GpsMode::AlwaysOn;
  original.operating_mode = OperatingMode::Offline;
  original.device_name = "my-device";
  original.use_fahrenheit = true;
  original.pm_use_usaqi = true;
  original.auto_lock_seconds = 30;

  REQUIRE(save_go_settings(store, original));
  REQUIRE(store.committed());

  GoSettings loaded = load_go_settings(store);

  REQUIRE(loaded.measure_interval_seconds == original.measure_interval_seconds);
  REQUIRE(loaded.inactivity_timeout_seconds == original.inactivity_timeout_seconds);
  REQUIRE(loaded.gps_interval_seconds == original.gps_interval_seconds);
  REQUIRE(loaded.gps_mode == original.gps_mode);
  REQUIRE(loaded.operating_mode == original.operating_mode);
  REQUIRE(loaded.device_name == original.device_name);
  REQUIRE(loaded.use_fahrenheit == original.use_fahrenheit);
  REQUIRE(loaded.pm_use_usaqi == original.pm_use_usaqi);
  REQUIRE(loaded.auto_lock_seconds == original.auto_lock_seconds);
}

TEST_CASE("round-trip preserves disable_cloud and static_ip", "[settings][stationary]") {
  FakeConfigStore store;

  GoSettings original;
  original.disable_cloud = true;
  original.static_ip.ip = 0x0100A8C0;          // 192.168.0.1 (LE)
  original.static_ip.netmask = 0x00FFFFFF;     // 255.255.255.0
  original.static_ip.gateway = 0xFE00A8C0;     // 192.168.0.254
  original.static_ip.dns_primary = 0x08080808; // 8.8.8.8
  original.static_ip.dns_secondary = 0x04040808;

  REQUIRE(save_go_settings(store, original));

  GoSettings loaded = load_go_settings(store);
  REQUIRE(loaded.disable_cloud == true);
  REQUIRE(loaded.static_ip.ip == original.static_ip.ip);
  REQUIRE(loaded.static_ip.netmask == original.static_ip.netmask);
  REQUIRE(loaded.static_ip.gateway == original.static_ip.gateway);
  REQUIRE(loaded.static_ip.dns_primary == original.static_ip.dns_primary);
  REQUIRE(loaded.static_ip.dns_secondary == original.static_ip.dns_secondary);
}

TEST_CASE("static_ip == 0 round-trips as DHCP", "[settings][stationary]") {
  // Re-provisioning DHCP must clear any previously stored static IP.
  FakeConfigStore store;

  GoSettings with_ip;
  with_ip.static_ip.ip = 0x0100A8C0;
  with_ip.static_ip.netmask = 0x00FFFFFF;
  REQUIRE(save_go_settings(store, with_ip));

  GoSettings dhcp;
  REQUIRE(save_go_settings(store, dhcp));

  GoSettings loaded = load_go_settings(store);
  REQUIRE(loaded.static_ip.ip == 0);
  REQUIRE(loaded.static_ip.netmask == 0);
}

TEST_CASE("round-trip preserves each GpsMode value", "[settings]") {
  FakeConfigStore store;
  GoSettings s;

  SECTION("AlwaysOff") {
    s.gps_mode = GpsMode::AlwaysOff;
    REQUIRE(save_go_settings(store, s));
    REQUIRE(load_go_settings(store).gps_mode == GpsMode::AlwaysOff);
  }

  SECTION("OnWhenTracking") {
    s.gps_mode = GpsMode::OnWhenTracking;
    REQUIRE(save_go_settings(store, s));
    REQUIRE(load_go_settings(store).gps_mode == GpsMode::OnWhenTracking);
  }

  SECTION("AlwaysOn") {
    s.gps_mode = GpsMode::AlwaysOn;
    REQUIRE(save_go_settings(store, s));
    REQUIRE(load_go_settings(store).gps_mode == GpsMode::AlwaysOn);
  }
}

TEST_CASE("round-trip preserves each OperatingMode value", "[settings]") {
  FakeConfigStore store;
  GoSettings s;

  SECTION("Portable") {
    s.operating_mode = OperatingMode::Portable;
    REQUIRE(save_go_settings(store, s));
    REQUIRE(load_go_settings(store).operating_mode == OperatingMode::Portable);
  }

  SECTION("Stationary") {
    s.operating_mode = OperatingMode::Stationary;
    REQUIRE(save_go_settings(store, s));
    REQUIRE(load_go_settings(store).operating_mode == OperatingMode::Stationary);
  }

  SECTION("Offline") {
    s.operating_mode = OperatingMode::Offline;
    REQUIRE(save_go_settings(store, s));
    REQUIRE(load_go_settings(store).operating_mode == OperatingMode::Offline);
  }
}

// ============================================================================
// Validation — save rejects out-of-range values
// ============================================================================

TEST_CASE("save rejects invalid measure_interval_seconds", "[settings][validation]") {
  FakeConfigStore store;
  GoSettings s;

  s.measure_interval_seconds = 0;
  REQUIRE_FALSE(save_go_settings(store, s));

  s.measure_interval_seconds = 3601;
  REQUIRE_FALSE(save_go_settings(store, s));
}

TEST_CASE("save rejects invalid inactivity_timeout_seconds", "[settings][validation]") {
  FakeConfigStore store;
  GoSettings s;

  s.inactivity_timeout_seconds = 4;
  REQUIRE_FALSE(save_go_settings(store, s));

  s.inactivity_timeout_seconds = 601;
  REQUIRE_FALSE(save_go_settings(store, s));
}

TEST_CASE("save rejects invalid gps_interval_seconds", "[settings][validation]") {
  FakeConfigStore store;
  GoSettings s;

  s.gps_interval_seconds = 0;
  REQUIRE_FALSE(save_go_settings(store, s));

  s.gps_interval_seconds = 61;
  REQUIRE_FALSE(save_go_settings(store, s));
}

TEST_CASE("save rejects invalid gps_mode", "[settings][validation]") {
  FakeConfigStore store;
  GoSettings s;

  // Force an out-of-range enum value
  s.gps_mode = static_cast<GpsMode>(3);
  REQUIRE_FALSE(save_go_settings(store, s));
}

TEST_CASE("save rejects invalid operating_mode", "[settings][validation]") {
  FakeConfigStore store;
  GoSettings s;

  s.operating_mode = static_cast<OperatingMode>(3);
  REQUIRE_FALSE(save_go_settings(store, s));
}

TEST_CASE("save rejects invalid auto_lock_seconds", "[settings][validation]") {
  FakeConfigStore store;
  GoSettings s;

  s.auto_lock_seconds = 5;
  REQUIRE_FALSE(save_go_settings(store, s));

  s.auto_lock_seconds = 15;
  REQUIRE_FALSE(save_go_settings(store, s));

  s.auto_lock_seconds = -1;
  REQUIRE_FALSE(save_go_settings(store, s));
}

TEST_CASE("save rejects invalid device_name", "[settings][validation]") {
  FakeConfigStore store;
  GoSettings s;

  s.device_name = "";
  REQUIRE_FALSE(save_go_settings(store, s));

  s.device_name = std::string(65, 'x');
  REQUIRE_FALSE(save_go_settings(store, s));
}

// ============================================================================
// Load ignores invalid stored values and keeps defaults
// ============================================================================

TEST_CASE("load ignores invalid stored values", "[settings][validation]") {
  FakeConfigStore store;

  // Pre-populate with out-of-range values
  GoSettings valid;
  REQUIRE(save_go_settings(store, valid));

  // Overwrite specific keys with invalid values
  store.set_int("mi", 0);     // below range
  store.set_int("gis", 999);  // above range
  store.set_int("gpm", 99);   // invalid enum
  store.set_int("opm", -1);   // invalid enum
  store.set_int("als", 42);   // not in allowed set
  store.set_int("ito", 3);    // below range
  store.set_string("dn", ""); // empty name

  GoSettings loaded = load_go_settings(store);

  // All should fall back to defaults
  REQUIRE(loaded.measure_interval_seconds == 10);
  REQUIRE(loaded.gps_interval_seconds == 5);
  REQUIRE(loaded.gps_mode == GpsMode::OnWhenTracking);
  REQUIRE(loaded.operating_mode == OperatingMode::Portable);
  REQUIRE(loaded.auto_lock_seconds == 10);
  REQUIRE(loaded.inactivity_timeout_seconds == 5);
  REQUIRE(loaded.device_name == "airgradient-go");
}

// ============================================================================
// Store write failure — save returns false, no commit
// ============================================================================

TEST_CASE("save returns false when store write fails", "[settings]") {
  FakeConfigStore store;
  store.set_fail_writes(true);

  GoSettings s;
  REQUIRE_FALSE(save_go_settings(store, s));
  REQUIRE_FALSE(store.committed());
}

// ============================================================================
// FactorySettings — fuel-gauge learning state persistence
// ============================================================================

TEST_CASE("factory settings load from empty store returns defaults", "[settings][factory]") {
  FakeConfigStore store;
  FactorySettings fs;
  REQUIRE(load_factory_settings(store, fs));
  REQUIRE(fs.fg_learning_stage == FgLearningStage::Idle);
  REQUIRE(fs.fg_learning_cycle == 0);
  REQUIRE(fs.fg_learning_itpor_losses == 0);
}

TEST_CASE("factory settings round-trip all fields", "[settings][factory]") {
  FakeConfigStore store;

  FactorySettings in;
  in.fg_learning_stage = FgLearningStage::Discharge;
  in.fg_learning_cycle = 2;
  in.fg_learning_itpor_losses = 1;
  REQUIRE(save_factory_settings(store, in));
  REQUIRE(store.committed());

  FactorySettings out;
  REQUIRE(load_factory_settings(store, out));
  REQUIRE(out.fg_learning_stage == FgLearningStage::Discharge);
  REQUIRE(out.fg_learning_cycle == 2);
  REQUIRE(out.fg_learning_itpor_losses == 1);
}

TEST_CASE("save_fg_learning_state writes and commits atomically", "[settings][factory]") {
  FakeConfigStore store;
  REQUIRE(save_fg_learning_state(store, FgLearningStage::CycleDone, 1, 0));
  REQUIRE(store.committed());

  FactorySettings out;
  REQUIRE(load_factory_settings(store, out));
  REQUIRE(out.fg_learning_stage == FgLearningStage::CycleDone);
  REQUIRE(out.fg_learning_cycle == 1);
}

TEST_CASE("save_fg_learning_state fails when store write fails", "[settings][factory]") {
  FakeConfigStore store;
  store.set_fail_writes(true);
  REQUIRE_FALSE(save_fg_learning_state(store, FgLearningStage::Charge, 1, 0));
  REQUIRE_FALSE(store.committed());
}

TEST_CASE("factory settings load ignores out-of-range stored stage", "[settings][factory]") {
  FakeConfigStore store;
  store.set_int("fs_s", 99); // invalid stage enum
  store.set_int("fs_c", 1);

  FactorySettings out;
  REQUIRE(load_factory_settings(store, out));
  REQUIRE(out.fg_learning_stage == FgLearningStage::Idle); // fell back to default
  REQUIRE(out.fg_learning_cycle == 1);                     // valid value still loaded
}

TEST_CASE("save_go_settings never touches factory keys", "[settings][factory]") {
  FakeConfigStore store;

  // Seed factory state.
  REQUIRE(save_fg_learning_state(store, FgLearningStage::Verify, 2, 1));

  // A user settings save must not overwrite or clear the factory keys.
  GoSettings s;
  REQUIRE(save_go_settings(store, s));

  FactorySettings out;
  REQUIRE(load_factory_settings(store, out));
  REQUIRE(out.fg_learning_stage == FgLearningStage::Verify);
  REQUIRE(out.fg_learning_cycle == 2);
  REQUIRE(out.fg_learning_itpor_losses == 1);
}

TEST_CASE("clear_factory_settings erases factory keys", "[settings][factory]") {
  FakeConfigStore store;
  REQUIRE(save_fg_learning_state(store, FgLearningStage::Complete, 2, 0));
  REQUIRE(store.has_int("fs_s"));

  REQUIRE(clear_factory_settings(store));
  REQUIRE_FALSE(store.has_int("fs_s"));
  REQUIRE_FALSE(store.has_int("fs_c"));
  REQUIRE_FALSE(store.has_int("fs_i"));

  // Subsequent load returns defaults.
  FactorySettings out;
  REQUIRE(load_factory_settings(store, out));
  REQUIRE(out.fg_learning_stage == FgLearningStage::Idle);
}

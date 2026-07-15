/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef SENSOR_MANAGER_H
#define SENSOR_MANAGER_H

#include <cstdint>

#include "measures_types.h"

#ifndef CONFIG_AVERAGING_ITERATION_INTERVAL_MS
#define CONFIG_AVERAGING_ITERATION_INTERVAL_MS 2000
#endif
#ifndef CONFIG_SENSOR_WARMUP_DURATION_MS
#define CONFIG_SENSOR_WARMUP_DURATION_MS 10000
#endif
#ifndef CONFIG_SENSOR_WARMUP_INTERVAL_MS
#define CONFIG_SENSOR_WARMUP_INTERVAL_MS 1000
#endif
// Derive the SGP41 gas-index sampling interval (ms) from the Kconfig choice.
// Sensirion supports 1 s and 10 s only; default is 10 s.
#if defined(CONFIG_SGP41_INDEX_SAMPLING_INTERVAL_1S)
static constexpr uint32_t SGP41_INDEX_SAMPLING_INTERVAL_MS = 1000;
#else
static constexpr uint32_t SGP41_INDEX_SAMPLING_INTERVAL_MS = 10000;
#endif

extern "C" {
#include "sensirion_gas_index_algorithm.h"
}

#include "hal/co2_sensor.h"
#include "hal/o3_no2_sensor.h"
#include "hal/pm_sensor.h"
#include "hal/pressure_sensor.h"
#include "hal/temp_hum_sensor.h"
#include "hal/tvoc_nox_sensor.h"

/**
 * @brief Bitmask selecting which sensor groups to poll in start_measures()
 *
 * Each bit gates a group of sensors. Only sensors in the requested groups
 * are read; unrequested groups return invalid sentinels in the result.
 */
enum class SensorGroup : uint8_t {
  None = 0x00,
  PM = 0x01,
  Other = 0x02,
  TvocNox = 0x04,
  All = 0x07,
};

inline SensorGroup operator|(SensorGroup a, SensorGroup b) {
  return static_cast<SensorGroup>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

inline bool has_group(SensorGroup mask, SensorGroup flag) {
  return (static_cast<uint8_t>(mask) & static_cast<uint8_t>(flag)) != 0;
}

/**
 * @brief Available sources for temp/hum fallback when no dedicated sensor
 */
enum class TempHumSource {
  DEDICATED, // TempHumSensor* temp_hum
  CO2,       // CO2Sensor* co2 (if supports_temp_hum)
  PM_A,      // PMSensor* pms_a (if supports_temp_hum)
  PRESSURE,  // PressureSensor* pressure (if supports_temp_hum)
};

/**
 * @brief Caller-configurable priority order for temp_hum_a fallback
 *
 * SensorManager resolves the highest-priority source that exists and supports
 * temp/hum before the iteration loop. Only that single source accumulates into
 * temp_hum_a. Default: DEDICATED > CO2 > PM_A > PRESSURE.
 */
struct TempHumFallbackConfig {
  static constexpr int MAX_SOURCES = 4;
  TempHumSource priority[MAX_SOURCES] = {
      TempHumSource::DEDICATED,
      TempHumSource::CO2,
      TempHumSource::PM_A,
      TempHumSource::PRESSURE,
  };
  int count = MAX_SOURCES;
};

struct Sensors {
  TempHumSensor *temp_hum;
  CO2Sensor *co2;
  PMSensor *pms_a;
  PMSensor *pms_b;
  TVOCNOxSensor *tvoc_nox;
  O3No2Sensor *o3_no2;
  PressureSensor *pressure;

  TempHumFallbackConfig temp_hum_a_fallback;
};

/// Result of a CO2 baseline calibration attempt.
enum class Co2CalibrationResult : uint8_t {
  Success,     ///< Calibration completed successfully
  Unsupported, ///< Sensor is absent or does not support calibration
  Failed,      ///< Calibration was started but did not complete in time
};

class SensorManager {
public:
  SensorManager(Sensors &sensors);
  ~SensorManager();

  Measures start_measures(int iterations, SensorGroup groups = SensorGroup::All);

  /// Run a blocking CO2 background calibration.
  ///
  /// Sends the calibration command and polls for completion.  Blocks for up
  /// to CALIBRATION_CHECK_INTERVAL_MS * CALIBRATION_MAX_ATTEMPTS ms.
  Co2CalibrationResult calibrate_co2();

  /// Run a single warmup cycle: TVOC/NOx conditioning + PM discard read.
  ///
  /// Does NOT pace itself — caller controls timing between calls.
  /// Returns immediately if no TVOC/NOx or PM sensors are present.
  void warmup_step();

  /// Run a full blocking warmup loop.
  ///
  /// Calls warmup_step() in a paced loop for CONFIG_SENSOR_WARMUP_DURATION_MS.
  /// Each iteration is paced to CONFIG_SENSOR_WARMUP_INTERVAL_MS.
  /// Returns immediately if no TVOC/NOx or PM sensors are present.
  void warmup();

  /// Put wired PM sensors into / out of low-power mode. No-op when none are
  /// wired or the sensor lacks a low-power mode.
  void pm_sleep();
  void pm_wake();

  /// True if a TVOC/NOx sensor is wired into this manager.
  bool has_tvoc_nox_sensor() const { return _sensors.tvoc_nox != nullptr; }

  /// Last temperature reading cached by the wired pressure sensor (updated on
  /// every pressure read — no bus traffic). Invalid sentinels when no pressure
  /// sensor is wired or it does not expose temperature.
  TempHumData pressure_temp_hum() const {
    if (_sensors.pressure != nullptr && _sensors.pressure->supports_temp_hum()) {
      return _sensors.pressure->temp_hum_data();
    }
    return TempHumData{};
  }

  /// Initialise the gas-index algorithm for the wired SGP41 sensor.
  /// @param sampling_interval_ms must equal the cadence at which the caller
  ///        invokes start_measures(SensorGroup::TvocNox). Sensirion supports
  ///        1000 ms or 10000 ms.
  ///
  /// Returns true and sets _index_configured = true on success. Returns
  /// false when no TVOC/NOx sensor is wired or the interval is unsupported.
  bool configure_tvoc_nox_index(uint32_t sampling_interval_ms);

  /// Update on-driver compensation for the wired TVOC/NOx sensor.
  /// Forwards to TVOCNOxSensor::set_compensation(). No-op when no
  /// TVOC/NOx sensor is wired.
  void set_tvoc_nox_compensation(float temperature_c, float humidity_pct);

private:
  Sensors &_sensors;

  // Gas-index algorithm state
  GasIndexAlgorithmParams _voc_params{};
  GasIndexAlgorithmParams _nox_params{};
  bool _index_configured = false;

  // CO2 calibration polling parameters
  static constexpr uint32_t CALIBRATION_CHECK_INTERVAL_MS = 5000;
  static constexpr int CALIBRATION_MAX_ATTEMPTS = 12;

  struct AverageMeasuresCounters {
    // TempHum counters
    int temp_a = 0;
    int hum_a = 0;
    int temp_b = 0;
    int hum_b = 0;
    // CO2 counter
    int co2 = 0;
    // PM sensor A counters
    int pm_a_01 = 0, pm_a_25 = 0, pm_a_10 = 0;
    int pm_a_01_sp = 0, pm_a_25_sp = 0, pm_a_10_sp = 0;
    int pm_a_03_pc = 0, pm_a_05_pc = 0, pm_a_01_pc = 0;
    int pm_a_25_pc = 0, pm_a_5_pc = 0, pm_a_10_pc = 0;
    // PM sensor B counters
    int pm_b_01 = 0, pm_b_25 = 0, pm_b_10 = 0;
    int pm_b_01_sp = 0, pm_b_25_sp = 0, pm_b_10_sp = 0;
    int pm_b_03_pc = 0, pm_b_05_pc = 0, pm_b_01_pc = 0;
    int pm_b_25_pc = 0, pm_b_5_pc = 0, pm_b_10_pc = 0;
    // TVOCNOx counters
    int tvoc_index = 0, tvoc_raw = 0;
    int nox_index = 0, nox_raw = 0;
    // O3No2 counters
    int o3_we = 0, o3_ae = 0;
    int no2_we = 0, no2_ae = 0;
    int afe_temp = 0;
    // Pressure counters
    int pressure = 0;
    int altitude = 0;
  };

  // Temp/hum fallback resolution
  TempHumSource _resolve_temp_hum_a_source();
  void _accumulate_temp_hum_a_fallback(TempHumSource source, TempHumData &sum_a,
                                       AverageMeasuresCounters &counters);

  // Accumulation methods
  void _accumulate_temp_hum(TempHumData &sum_a, TempHumData &sum_b,
                            AverageMeasuresCounters &counters);
  void _accumulate_co2(CO2Data &sum, AverageMeasuresCounters &counters);
  void _accumulate_pm_sensor(PMSensor *sensor, PMData &sum, AverageMeasuresCounters &counters,
                             TempHumData &temp_hum_sum_b, bool is_sensor_a,
                             bool sensor_supports_temp_hum);
  void _accumulate_tvoc_nox(TVOCNOxData &sum, AverageMeasuresCounters &counters);
  void _accumulate_o3_no2(O3No2Data &sum, AverageMeasuresCounters &counters);
  void _accumulate_pressure(PressureData &sum, AverageMeasuresCounters &counters);

  // Averaging methods
  TempHumData _calculate_temp_hum_a_average(const TempHumData &sum,
                                            const AverageMeasuresCounters &counters);
  TempHumData _calculate_temp_hum_b_average(const TempHumData &sum,
                                            const AverageMeasuresCounters &counters);
  CO2Data _calculate_co2_average(const CO2Data &sum, const AverageMeasuresCounters &counters);
  PMData _calculate_pm_average(const PMData &sum, const AverageMeasuresCounters &counters,
                               bool is_sensor_a);
  TVOCNOxData _calculate_tvoc_nox_average(const TVOCNOxData &sum,
                                          const AverageMeasuresCounters &counters);
  O3No2Data _calculate_o3_no2_average(const O3No2Data &sum,
                                      const AverageMeasuresCounters &counters);
  PressureData _calculate_pressure_average(const PressureData &sum,
                                           const AverageMeasuresCounters &counters);
};
#endif // !SENSOR_MANAGER_H

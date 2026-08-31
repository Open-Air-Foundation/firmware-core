/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "services/sensor_manager.h"

#include "ag_log.h"
#include "measures_types.h"
#include "rtos.h"

extern "C" {
#include "sensirion_gas_index_algorithm.h"
}

static constexpr const char *TAG = "SensorManager";

namespace {

constexpr int LEARNING_OFFSET_HOURS_MIN = 1;
constexpr int LEARNING_OFFSET_HOURS_MAX = 1000;

bool is_learning_offset_valid(int value) {
  return value >= LEARNING_OFFSET_HOURS_MIN && value <= LEARNING_OFFSET_HOURS_MAX;
}

bool set_learning_offset(GasIndexAlgorithmParams &params, int learning_offset) {
  int32_t index_offset = 0;
  int32_t current_learning_offset = 0;
  int32_t learning_gain = 0;
  int32_t gating_duration = 0;
  int32_t initial_std = 0;
  int32_t gain_factor = 0;
  GasIndexAlgorithm_get_tuning_parameters(&params, &index_offset, &current_learning_offset,
                                          &learning_gain, &gating_duration, &initial_std,
                                          &gain_factor);
  if (current_learning_offset == learning_offset) {
    return false;
  }

  GasIndexAlgorithm_set_tuning_parameters(&params, index_offset, learning_offset, learning_gain,
                                          gating_duration, initial_std, gain_factor);
  return true;
}

} // namespace

SensorManager::SensorManager(Sensors &sensor) : _sensors(sensor) {}

SensorManager::~SensorManager() {}

void SensorManager::warmup_step() {
  if (!_sensors.tvoc_nox && !_sensors.pms_a && !_sensors.pms_b) {
    return;
  }

  if (_sensors.tvoc_nox) {
    if (!_sensors.tvoc_nox->run_conditioning()) {
      AG_LOGW(TAG, "TVOC/NOx conditioning failed");
    }
  }

  PMData discard;
  if (_sensors.pms_a) {
    (void)_sensors.pms_a->read(discard);
  }
  if (_sensors.pms_b) {
    (void)_sensors.pms_b->read(discard);
  }
}

void SensorManager::warmup() {
  if (!_sensors.tvoc_nox && !_sensors.pms_a && !_sensors.pms_b) {
    return;
  }

  const int iterations = CONFIG_SENSOR_WARMUP_DURATION_MS / CONFIG_SENSOR_WARMUP_INTERVAL_MS;
  AG_LOGI(TAG, "warmup: %d iterations (%d ms interval)", iterations,
          CONFIG_SENSOR_WARMUP_INTERVAL_MS);
  for (int i = 0; i < iterations; i++) {
    AG_LOGI(TAG, "warmup: iteration %d/%d", i + 1, iterations);
    uint64_t start_time_ms = RTOS::get_time_ms();

    warmup_step();

    uint64_t elapsed_time_ms = RTOS::get_time_ms() - start_time_ms;
    if (elapsed_time_ms < CONFIG_SENSOR_WARMUP_INTERVAL_MS) {
      uint32_t delay_ms = static_cast<uint32_t>(CONFIG_SENSOR_WARMUP_INTERVAL_MS - elapsed_time_ms);
      RTOS::delay_ms(delay_ms);
    }
  }
}

void SensorManager::pm_sleep() {
  if (_sensors.pms_a) {
    _sensors.pms_a->sleep();
  }
  if (_sensors.pms_b) {
    _sensors.pms_b->sleep();
  }
}

void SensorManager::pm_wake() {
  if (_sensors.pms_a) {
    _sensors.pms_a->wake();
  }
  if (_sensors.pms_b) {
    _sensors.pms_b->wake();
  }
}

Co2CalibrationResult SensorManager::calibrate_co2() {
  if (!_sensors.co2 || !_sensors.co2->supports_calibration()) {
    return Co2CalibrationResult::Unsupported;
  }

  if (!_sensors.co2->do_baseline_calibration()) {
    return Co2CalibrationResult::Failed;
  }

  for (int attempt = 0; attempt < CALIBRATION_MAX_ATTEMPTS; attempt++) {
    RTOS::delay_ms(CALIBRATION_CHECK_INTERVAL_MS);
    if (_sensors.co2->is_baseline_calibration_done()) {
      return Co2CalibrationResult::Success;
    }
  }

  return Co2CalibrationResult::Failed;
}

Co2AbcPeriodResult SensorManager::set_co2_abc_period_days(int days) {
  if (!_sensors.co2 || !_sensors.co2->supports_abc_period_configuration()) {
    return Co2AbcPeriodResult::Unsupported;
  }

  return _sensors.co2->set_abc_period_days(days) ? Co2AbcPeriodResult::Success
                                                 : Co2AbcPeriodResult::Failed;
}

bool SensorManager::configure_tvoc_nox_index(uint32_t sampling_interval_ms,
                                             int tvoc_learning_offset, int nox_learning_offset) {
  _index_configured = false;

  if (!_sensors.tvoc_nox) {
    AG_LOGW(TAG, "configure_tvoc_nox_index: no TVOC/NOx sensor wired");
    return false;
  }

  if (sampling_interval_ms != 1000 && sampling_interval_ms != 10000) {
    AG_LOGE(TAG, "configure_tvoc_nox_index: unsupported interval %lu ms",
            static_cast<unsigned long>(sampling_interval_ms));
    return false;
  }
  if (!is_learning_offset_valid(tvoc_learning_offset) ||
      !is_learning_offset_valid(nox_learning_offset)) {
    AG_LOGE(TAG, "configure_tvoc_nox_index: invalid learning offsets tvoc=%d nox=%d",
            tvoc_learning_offset, nox_learning_offset);
    return false;
  }

  const float sampling_interval_s = static_cast<float>(sampling_interval_ms) / 1000.0f;
  GasIndexAlgorithm_init_with_sampling_interval(&_voc_params, GasIndexAlgorithm_ALGORITHM_TYPE_VOC,
                                                sampling_interval_s);
  GasIndexAlgorithm_init_with_sampling_interval(&_nox_params, GasIndexAlgorithm_ALGORITHM_TYPE_NOX,
                                                sampling_interval_s);
  (void)set_learning_offset(_voc_params, tvoc_learning_offset);
  (void)set_learning_offset(_nox_params, nox_learning_offset);

  _index_configured = true;
  AG_LOGI(TAG, "gas-index algorithm configured (%.0f s interval, tvoc=%d h, nox=%d h)",
          sampling_interval_s, tvoc_learning_offset, nox_learning_offset);
  return true;
}

TvocNoxLearningOffsetResult SensorManager::set_tvoc_nox_learning_offsets(int tvoc_learning_offset,
                                                                         int nox_learning_offset) {
  if (!_sensors.tvoc_nox || !_index_configured) {
    return TvocNoxLearningOffsetResult::Unsupported;
  }
  if (!is_learning_offset_valid(tvoc_learning_offset) ||
      !is_learning_offset_valid(nox_learning_offset)) {
    return TvocNoxLearningOffsetResult::Failed;
  }

  const bool tvoc_changed = set_learning_offset(_voc_params, tvoc_learning_offset);
  const bool nox_changed = set_learning_offset(_nox_params, nox_learning_offset);
  AG_LOGI(TAG, "gas-index learning offsets tvoc=%d h%s, nox=%d h%s", tvoc_learning_offset,
          tvoc_changed ? " applied" : " unchanged", nox_learning_offset,
          nox_changed ? " applied" : " unchanged");
  return TvocNoxLearningOffsetResult::Success;
}

void SensorManager::set_tvoc_nox_compensation(float temperature_c, float humidity_pct) {
  if (_sensors.tvoc_nox) {
    _sensors.tvoc_nox->set_compensation(temperature_c, humidity_pct);
  }
}

Measures SensorManager::start_measures(int iterations, SensorGroup groups) {
  // Initialize accumulation variables
  TempHumData sum_temp_hum_a = {0, 0};
  TempHumData sum_temp_hum_b = {0, 0};
  CO2Data sum_co2 = {0};
  PMData sum_pm_a = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  PMData sum_pm_b = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  TVOCNOxData sum_voc_nox = {0, 0, 0, 0};
  O3No2Data sum_o3no2 = {0, 0, 0, 0, 0};
  PressureData sum_pressure = {0, 0, 0};

  // Initialize single flattened counter struct
  AverageMeasuresCounters counters;

  // Resolve which single source provides temp_hum_a (priority-based, once before loop)
  TempHumSource temp_hum_a_source = _resolve_temp_hum_a_source();

  // Cache PM sensor B temp/hum capability (temp_hum_b is always from PM_B only)
  bool pms_b_supports_temp_hum = _sensors.pms_b && _sensors.pms_b->supports_temp_hum();

  // Accumulate readings over multiple iterations
  for (int i = 0; i < iterations; i++) {
    // Capture start time for 2-second iteration timing
    uint64_t start_time_ms = RTOS::get_time_ms();

    if (has_group(groups, SensorGroup::Other)) {
      // When temp/hum_a falls back to CO2 or pressure, read that sensor first so
      // temp_hum_data() returns the same-iteration cached values. Future
      // improvement: only consume fallback temp/hum when that source read
      // succeeds in this iteration, to avoid using stale cached data.
      if (temp_hum_a_source == TempHumSource::CO2) {
        _accumulate_co2(sum_co2, counters);
      }
      if (temp_hum_a_source == TempHumSource::PRESSURE) {
        _accumulate_pressure(sum_pressure, counters);
      }

      _accumulate_temp_hum_a_fallback(temp_hum_a_source, sum_temp_hum_a, counters);
      _accumulate_o3_no2(sum_o3no2, counters);

      if (temp_hum_a_source != TempHumSource::CO2) {
        _accumulate_co2(sum_co2, counters);
      }
      if (temp_hum_a_source != TempHumSource::PRESSURE) {
        _accumulate_pressure(sum_pressure, counters);
      }
    }

    if (has_group(groups, SensorGroup::TvocNox)) {
      _accumulate_tvoc_nox(sum_voc_nox, counters);
    }

    if (has_group(groups, SensorGroup::PM)) {
      _accumulate_pm_sensor(_sensors.pms_a, sum_pm_a, counters, sum_temp_hum_b, true, false);
      _accumulate_pm_sensor(_sensors.pms_b, sum_pm_b, counters, sum_temp_hum_b, false,
                            pms_b_supports_temp_hum);
    }

    // Skip delay for single-iteration measurements
    if (iterations > 1) {
      uint64_t elapsed_time_ms = RTOS::get_time_ms() - start_time_ms;
      const uint64_t target_iteration_time_ms = CONFIG_AVERAGING_ITERATION_INTERVAL_MS;
      if (elapsed_time_ms < target_iteration_time_ms) {
        uint32_t delay_ms = static_cast<uint32_t>(target_iteration_time_ms - elapsed_time_ms);
        RTOS::delay_ms(delay_ms);
      }
    }
  }

  // Calculate averages
  Measures measures;
  measures.temp_hum_a = _calculate_temp_hum_a_average(sum_temp_hum_a, counters);
  measures.temp_hum_b = _calculate_temp_hum_b_average(sum_temp_hum_b, counters);
  measures.co2 = _calculate_co2_average(sum_co2, counters);
  measures.pm_a = _calculate_pm_average(sum_pm_a, counters, true);
  measures.pm_b = _calculate_pm_average(sum_pm_b, counters, false);
  measures.tvoc_nox = _calculate_tvoc_nox_average(sum_voc_nox, counters);
  measures.electrode = _calculate_o3_no2_average(sum_o3no2, counters);
  measures.pressure = _calculate_pressure_average(sum_pressure, counters);

  return measures;
}

void SensorManager::_accumulate_temp_hum(TempHumData &sum_a, TempHumData &sum_b,
                                         AverageMeasuresCounters &counters) {
  if (!_sensors.temp_hum) {
    return;
  }

  TempHumData data;
  if (_sensors.temp_hum->read(data)) {
    if (data.is_temp_valid()) {
      sum_a.temperature += data.temperature;
      counters.temp_a++;
    }
    if (data.is_hum_valid()) {
      sum_a.humidity += data.humidity;
      counters.hum_a++;
    }
  }
  // Note: sum_b is not modified - dedicated sensor only populates temp_hum_a
}

void SensorManager::_accumulate_co2(CO2Data &sum, AverageMeasuresCounters &counters) {
  if (!_sensors.co2) {
    return;
  }

  CO2Data data;
  if (_sensors.co2->read(data)) {
    if (data.is_valid()) {
      sum.co2 += data.co2;
      counters.co2++;
    }
  }
}

void SensorManager::_accumulate_pm_sensor(PMSensor *sensor, PMData &sum,
                                          AverageMeasuresCounters &counters,
                                          TempHumData &temp_hum_sum_b, bool is_sensor_a,
                                          bool sensor_supports_temp_hum) {
  if (!sensor) {
    return;
  }

  PMData data;
  if (sensor->read(data)) {
    // Choose the appropriate counter fields based on which sensor this is
    if (is_sensor_a) {
      if (data.is_pm_01_valid()) {
        sum.pm_01 += data.pm_01;
        counters.pm_a_01++;
      }
      if (data.is_pm_25_valid()) {
        sum.pm_25 += data.pm_25;
        counters.pm_a_25++;
      }
      if (data.is_pm_10_valid()) {
        sum.pm_10 += data.pm_10;
        counters.pm_a_10++;
      }
      if (data.is_pm_01_sp_valid()) {
        sum.pm_01_sp += data.pm_01_sp;
        counters.pm_a_01_sp++;
      }
      if (data.is_pm_25_sp_valid()) {
        sum.pm_25_sp += data.pm_25_sp;
        counters.pm_a_25_sp++;
      }
      if (data.is_pm_10_sp_valid()) {
        sum.pm_10_sp += data.pm_10_sp;
        counters.pm_a_10_sp++;
      }
      if (data.is_pm_03_pc_valid()) {
        sum.pm_03_pc += data.pm_03_pc;
        counters.pm_a_03_pc++;
      }
      if (data.is_pm_05_pc_valid()) {
        sum.pm_05_pc += data.pm_05_pc;
        counters.pm_a_05_pc++;
      }
      if (data.is_pm_01_pc_valid()) {
        sum.pm_01_pc += data.pm_01_pc;
        counters.pm_a_01_pc++;
      }
      if (data.is_pm_25_pc_valid()) {
        sum.pm_25_pc += data.pm_25_pc;
        counters.pm_a_25_pc++;
      }
      if (data.is_pm_5_pc_valid()) {
        sum.pm_5_pc += data.pm_5_pc;
        counters.pm_a_5_pc++;
      }
      if (data.is_pm_10_pc_valid()) {
        sum.pm_10_pc += data.pm_10_pc;
        counters.pm_a_10_pc++;
      }
    } else {
      // Sensor B
      if (data.is_pm_01_valid()) {
        sum.pm_01 += data.pm_01;
        counters.pm_b_01++;
      }
      if (data.is_pm_25_valid()) {
        sum.pm_25 += data.pm_25;
        counters.pm_b_25++;
      }
      if (data.is_pm_10_valid()) {
        sum.pm_10 += data.pm_10;
        counters.pm_b_10++;
      }
      if (data.is_pm_01_sp_valid()) {
        sum.pm_01_sp += data.pm_01_sp;
        counters.pm_b_01_sp++;
      }
      if (data.is_pm_25_sp_valid()) {
        sum.pm_25_sp += data.pm_25_sp;
        counters.pm_b_25_sp++;
      }
      if (data.is_pm_10_sp_valid()) {
        sum.pm_10_sp += data.pm_10_sp;
        counters.pm_b_10_sp++;
      }
      if (data.is_pm_03_pc_valid()) {
        sum.pm_03_pc += data.pm_03_pc;
        counters.pm_b_03_pc++;
      }
      if (data.is_pm_05_pc_valid()) {
        sum.pm_05_pc += data.pm_05_pc;
        counters.pm_b_05_pc++;
      }
      if (data.is_pm_01_pc_valid()) {
        sum.pm_01_pc += data.pm_01_pc;
        counters.pm_b_01_pc++;
      }
      if (data.is_pm_25_pc_valid()) {
        sum.pm_25_pc += data.pm_25_pc;
        counters.pm_b_25_pc++;
      }
      if (data.is_pm_5_pc_valid()) {
        sum.pm_5_pc += data.pm_5_pc;
        counters.pm_b_5_pc++;
      }
      if (data.is_pm_10_pc_valid()) {
        sum.pm_10_pc += data.pm_10_pc;
        counters.pm_b_10_pc++;
      }
    }

    // PM sensor B independently populates temp_hum_b when no dedicated sensor
    if (!is_sensor_a && _sensors.temp_hum == nullptr && sensor_supports_temp_hum) {
      TempHumData th = sensor->temp_hum_data();
      if (th.is_temp_valid()) {
        temp_hum_sum_b.temperature += th.temperature;
        counters.temp_b++;
      }
      if (th.is_hum_valid()) {
        temp_hum_sum_b.humidity += th.humidity;
        counters.hum_b++;
      }
    }
  }
}

void SensorManager::_accumulate_tvoc_nox(TVOCNOxData &sum, AverageMeasuresCounters &counters) {
  if (!_sensors.tvoc_nox) {
    return;
  }

  TVOCNOxData data;
  if (!_sensors.tvoc_nox->read(data)) {
    return;
  }

  const bool tvoc_raw_valid = data.is_tvoc_raw_valid();
  const bool nox_raw_valid = data.is_nox_raw_valid();

  // Raw accumulation
  if (tvoc_raw_valid) {
    sum.tvoc_raw += data.tvoc_raw;
    counters.tvoc_raw++;
  }
  if (nox_raw_valid) {
    sum.nox_raw += data.nox_raw;
    counters.nox_raw++;
  }

  // Algorithm step — gated by _index_configured. Fast path never
  // configures the algorithm, so this branch is skipped there and
  // _last_tvoc_nox only carries raw values into the cache.
  if (_index_configured) {
    if (tvoc_raw_valid) {
      int32_t voc_idx = 0;
      GasIndexAlgorithm_process(&_voc_params, data.tvoc_raw, &voc_idx);
      // Sensirion returns 0 during the 45 s blackout. Do not average the
      // invalid sentinel; simply skip the field until a nonzero index exists.
      if (voc_idx != 0) {
        sum.tvoc_index += voc_idx;
        counters.tvoc_index++;
      }
    }
    if (nox_raw_valid) {
      int32_t nox_idx = 0;
      GasIndexAlgorithm_process(&_nox_params, data.nox_raw, &nox_idx);
      if (nox_idx != 0) {
        sum.nox_index += nox_idx;
        counters.nox_index++;
      }
    }
  } else {
    // Algorithm not configured — pass through driver-reported index
    // fields (which are invalid sentinels from the SGP41 driver).
    if (data.is_tvoc_index_valid()) {
      sum.tvoc_index += data.tvoc_index;
      counters.tvoc_index++;
    }
    if (data.is_nox_index_valid()) {
      sum.nox_index += data.nox_index;
      counters.nox_index++;
    }
  }
}

void SensorManager::_accumulate_o3_no2(O3No2Data &sum, AverageMeasuresCounters &counters) {
  if (!_sensors.o3_no2) {
    return;
  }

  O3No2Data data;
  if (_sensors.o3_no2->read(data)) {
    if (data.is_o3_working_valid()) {
      sum.o3_we += data.o3_we;
      counters.o3_we++;
    }
    if (data.is_o3_auxiliary_valid()) {
      sum.o3_ae += data.o3_ae;
      counters.o3_ae++;
    }
    if (data.is_no2_working_valid()) {
      sum.no2_we += data.no2_we;
      counters.no2_we++;
    }
    if (data.is_no2_auxiliary_valid()) {
      sum.no2_ae += data.no2_ae;
      counters.no2_ae++;
    }
    if (data.is_afe_temp_valid()) {
      sum.afe_temp += data.afe_temp;
      counters.afe_temp++;
    }
  }
}

TempHumData SensorManager::_calculate_temp_hum_a_average(const TempHumData &sum,
                                                         const AverageMeasuresCounters &counters) {
  return {.temperature = (counters.temp_a > 0) ? sum.temperature / counters.temp_a
                                               : MeasuresInvalid::TEMPERATURE,
          .humidity =
              (counters.hum_a > 0) ? sum.humidity / counters.hum_a : MeasuresInvalid::HUMIDITY};
}

TempHumData SensorManager::_calculate_temp_hum_b_average(const TempHumData &sum,
                                                         const AverageMeasuresCounters &counters) {
  return {.temperature = (counters.temp_b > 0) ? sum.temperature / counters.temp_b
                                               : MeasuresInvalid::TEMPERATURE,
          .humidity =
              (counters.hum_b > 0) ? sum.humidity / counters.hum_b : MeasuresInvalid::HUMIDITY};
}

CO2Data SensorManager::_calculate_co2_average(const CO2Data &sum,
                                              const AverageMeasuresCounters &counters) {
  return {.co2 = (counters.co2 > 0) ? sum.co2 / counters.co2 : MeasuresInvalid::CO2};
}

PMData SensorManager::_calculate_pm_average(const PMData &sum,
                                            const AverageMeasuresCounters &counters,
                                            bool is_sensor_a) {
  if (is_sensor_a) {
    return {.pm_01 = (counters.pm_a_01 > 0) ? sum.pm_01 / counters.pm_a_01 : MeasuresInvalid::PM,
            .pm_25 = (counters.pm_a_25 > 0) ? sum.pm_25 / counters.pm_a_25 : MeasuresInvalid::PM,
            .pm_10 = (counters.pm_a_10 > 0) ? sum.pm_10 / counters.pm_a_10 : MeasuresInvalid::PM,
            .pm_01_sp = (counters.pm_a_01_sp > 0) ? sum.pm_01_sp / counters.pm_a_01_sp
                                                  : MeasuresInvalid::PM,
            .pm_25_sp = (counters.pm_a_25_sp > 0) ? sum.pm_25_sp / counters.pm_a_25_sp
                                                  : MeasuresInvalid::PM,
            .pm_10_sp = (counters.pm_a_10_sp > 0) ? sum.pm_10_sp / counters.pm_a_10_sp
                                                  : MeasuresInvalid::PM,
            .pm_03_pc = (counters.pm_a_03_pc > 0) ? sum.pm_03_pc / counters.pm_a_03_pc
                                                  : MeasuresInvalid::PM,
            .pm_05_pc = (counters.pm_a_05_pc > 0) ? sum.pm_05_pc / counters.pm_a_05_pc
                                                  : MeasuresInvalid::PM,
            .pm_01_pc = (counters.pm_a_01_pc > 0) ? sum.pm_01_pc / counters.pm_a_01_pc
                                                  : MeasuresInvalid::PM,
            .pm_25_pc = (counters.pm_a_25_pc > 0) ? sum.pm_25_pc / counters.pm_a_25_pc
                                                  : MeasuresInvalid::PM,
            .pm_5_pc =
                (counters.pm_a_5_pc > 0) ? sum.pm_5_pc / counters.pm_a_5_pc : MeasuresInvalid::PM,
            .pm_10_pc = (counters.pm_a_10_pc > 0) ? sum.pm_10_pc / counters.pm_a_10_pc
                                                  : MeasuresInvalid::PM};
  } else {
    return {.pm_01 = (counters.pm_b_01 > 0) ? sum.pm_01 / counters.pm_b_01 : MeasuresInvalid::PM,
            .pm_25 = (counters.pm_b_25 > 0) ? sum.pm_25 / counters.pm_b_25 : MeasuresInvalid::PM,
            .pm_10 = (counters.pm_b_10 > 0) ? sum.pm_10 / counters.pm_b_10 : MeasuresInvalid::PM,
            .pm_01_sp = (counters.pm_b_01_sp > 0) ? sum.pm_01_sp / counters.pm_b_01_sp
                                                  : MeasuresInvalid::PM,
            .pm_25_sp = (counters.pm_b_25_sp > 0) ? sum.pm_25_sp / counters.pm_b_25_sp
                                                  : MeasuresInvalid::PM,
            .pm_10_sp = (counters.pm_b_10_sp > 0) ? sum.pm_10_sp / counters.pm_b_10_sp
                                                  : MeasuresInvalid::PM,
            .pm_03_pc = (counters.pm_b_03_pc > 0) ? sum.pm_03_pc / counters.pm_b_03_pc
                                                  : MeasuresInvalid::PM,
            .pm_05_pc = (counters.pm_b_05_pc > 0) ? sum.pm_05_pc / counters.pm_b_05_pc
                                                  : MeasuresInvalid::PM,
            .pm_01_pc = (counters.pm_b_01_pc > 0) ? sum.pm_01_pc / counters.pm_b_01_pc
                                                  : MeasuresInvalid::PM,
            .pm_25_pc = (counters.pm_b_25_pc > 0) ? sum.pm_25_pc / counters.pm_b_25_pc
                                                  : MeasuresInvalid::PM,
            .pm_5_pc =
                (counters.pm_b_5_pc > 0) ? sum.pm_5_pc / counters.pm_b_5_pc : MeasuresInvalid::PM,
            .pm_10_pc = (counters.pm_b_10_pc > 0) ? sum.pm_10_pc / counters.pm_b_10_pc
                                                  : MeasuresInvalid::PM};
  }
}

TVOCNOxData SensorManager::_calculate_tvoc_nox_average(const TVOCNOxData &sum,
                                                       const AverageMeasuresCounters &counters) {
  return {.tvoc_index = (counters.tvoc_index > 0) ? sum.tvoc_index / counters.tvoc_index
                                                  : MeasuresInvalid::TVOC,
          .tvoc_raw =
              (counters.tvoc_raw > 0) ? sum.tvoc_raw / counters.tvoc_raw : MeasuresInvalid::TVOC,
          .nox_index =
              (counters.nox_index > 0) ? sum.nox_index / counters.nox_index : MeasuresInvalid::NOX,
          .nox_raw =
              (counters.nox_raw > 0) ? sum.nox_raw / counters.nox_raw : MeasuresInvalid::NOX};
}

O3No2Data SensorManager::_calculate_o3_no2_average(const O3No2Data &sum,
                                                   const AverageMeasuresCounters &counters) {
  return {.o3_we = (counters.o3_we > 0) ? sum.o3_we / counters.o3_we : MeasuresInvalid::VOLT,
          .o3_ae = (counters.o3_ae > 0) ? sum.o3_ae / counters.o3_ae : MeasuresInvalid::VOLT,
          .no2_we = (counters.no2_we > 0) ? sum.no2_we / counters.no2_we : MeasuresInvalid::VOLT,
          .no2_ae = (counters.no2_ae > 0) ? sum.no2_ae / counters.no2_ae : MeasuresInvalid::VOLT,
          .afe_temp =
              (counters.afe_temp > 0) ? sum.afe_temp / counters.afe_temp : MeasuresInvalid::VOLT};
}

void SensorManager::_accumulate_pressure(PressureData &sum, AverageMeasuresCounters &counters) {
  if (!_sensors.pressure) {
    return;
  }

  PressureData data;
  if (_sensors.pressure->read(data)) {
    if (_sensors.pressure->supports_temp_hum()) {
      const auto &die = _sensors.pressure->temp_hum_data();
      if (die.is_temp_valid()) {
        sum.temperature += die.temperature;
        counters.pressure_temp++;
      }
    }
    if (data.is_pressure_valid()) {
      sum.pressure += data.pressure;
      counters.pressure++;
    }
    if (data.is_altitude_valid()) {
      sum.altitude += data.altitude;
      counters.altitude++;
    }
  }
}

PressureData SensorManager::_calculate_pressure_average(const PressureData &sum,
                                                        const AverageMeasuresCounters &counters) {
  return {.pressure = (counters.pressure > 0) ? sum.pressure / counters.pressure
                                              : MeasuresInvalid::PRESSURE,
          .altitude = (counters.altitude > 0) ? sum.altitude / counters.altitude
                                              : MeasuresInvalid::ALTITUDE,
          .temperature = (counters.pressure_temp > 0)
                             ? sum.temperature / counters.pressure_temp
                             : MeasuresInvalid::TEMPERATURE};
}

TempHumSource SensorManager::_resolve_temp_hum_a_source() {
  const auto &cfg = _sensors.temp_hum_a_fallback;

  for (int i = 0; i < cfg.count; i++) {
    switch (cfg.priority[i]) {
    case TempHumSource::DEDICATED:
      if (_sensors.temp_hum) {
        return TempHumSource::DEDICATED;
      }
      break;
    case TempHumSource::CO2:
      if (_sensors.co2 && _sensors.co2->supports_temp_hum()) {
        return TempHumSource::CO2;
      }
      break;
    case TempHumSource::PM_A:
      if (_sensors.pms_a && _sensors.pms_a->supports_temp_hum()) {
        return TempHumSource::PM_A;
      }
      break;
    case TempHumSource::PRESSURE:
      if (_sensors.pressure && _sensors.pressure->supports_temp_hum()) {
        return TempHumSource::PRESSURE;
      }
      break;
    }
  }

  // No source available — temp_hum_a will remain at invalid sentinels
  return TempHumSource::DEDICATED;
}

void SensorManager::_accumulate_temp_hum_a_fallback(TempHumSource source, TempHumData &sum_a,
                                                    AverageMeasuresCounters &counters) {
  TempHumData th;

  switch (source) {
  case TempHumSource::DEDICATED:
    _accumulate_temp_hum(sum_a, sum_a, counters);
    return;

  case TempHumSource::CO2:
    if (!_sensors.co2) {
      return;
    }
    th = _sensors.co2->temp_hum_data();
    break;

  case TempHumSource::PM_A:
    if (!_sensors.pms_a) {
      return;
    }
    th = _sensors.pms_a->temp_hum_data();
    break;

  case TempHumSource::PRESSURE:
    if (!_sensors.pressure) {
      return;
    }
    th = _sensors.pressure->temp_hum_data();
    break;
  }

  if (th.is_temp_valid()) {
    sum_a.temperature += th.temperature;
    counters.temp_a++;
  }
  if (th.is_hum_valid()) {
    sum_a.humidity += th.humidity;
    counters.hum_a++;
  }
}

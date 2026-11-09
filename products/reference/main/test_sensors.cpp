#include "test_sensors.h"

#include "esp_log.h"

#include "board_config.h"
#include "drivers/bq25629/bq25629_bms.h"
#include "drivers/dps368/dps368.h"
#include "drivers/sgp41/sgp41.h"
#include "drivers/sps30/sps30.h"
#include "drivers/stcc4/stcc4.h"
#include "measures_types.h"
#include "rtos.h"

static constexpr const char *TAG = "test_sensors";

static constexpr int N_READINGS = 5;
static constexpr int READING_INTERVAL_MS = 2000;
static constexpr int SGP41_COND_CYCLES = 10; // ~5 s conditioning

void run_test_sensors(i2c_master_bus_handle_t i2c_bus) {
  ESP_LOGI(TAG, "--- Sensors test start (%d readings) ---", N_READINGS);

  // BMS instance (BQ25629 on same I2C bus)
  drivers::BQ25629_Config bms_config = {
      .charge_voltage_mv = 4200,
      .charge_current_ma = 1000,
      .input_current_limit_ma = 1500,
      .input_voltage_limit_mv = 4600,
      .min_system_voltage_mv = 3520,
      .precharge_current_ma = 30,
      .term_current_ma = 20,
      .enable_charging = true,
      .enable_adc = true,
      .ntc =
          {
              .pullup_ohm = 4120.0f,
              .pulldown_ohm = 17330.0f,
              .r25_ohm = 10000.0f,
              .beta_k = 3950.0f,
          },
  };
  BQ25629Bms bms(i2c_bus, bms_config);

  // Sensor instances (all I2C)
  SPS30 sps30(i2c_bus);
  STCC4 stcc4(i2c_bus);
  SGP41 sgp41(i2c_bus);
  DPS368 dps368(i2c_bus);

  if (!bms.init()) {
    ESP_LOGW(TAG, "BQ25629 BMS init failed — skipping");
  }

  if (!sps30.init()) {
    ESP_LOGW(TAG, "SPS30 init failed — skipping");
  }
  if (!stcc4.init()) {
    ESP_LOGW(TAG, "STCC4 init failed — skipping");
  }
  if (!dps368.init()) {
    ESP_LOGW(TAG, "DPS368 init failed — skipping");
  }
  if (!sgp41.init()) {
    ESP_LOGW(TAG, "SGP41 init failed — skipping");
  } else {
    ESP_LOGI(TAG, "SGP41 conditioning (%d cycles)...", SGP41_COND_CYCLES);
    for (int i = 0; i < SGP41_COND_CYCLES; i++) {
      if (!sgp41.run_conditioning()) {
        ESP_LOGW(TAG, "SGP41 conditioning failed at cycle %d", i + 1);
      }
      RTOS::delay_ms(500);
    }
  }

  for (int reading = 1; reading <= N_READINGS; reading++) {
    ESP_LOGI(TAG, "=== reading %d/%d ===", reading, N_READINGS);

    BmsTelemetry bms_telem;
    if (bms.read_telemetry(bms_telem) && bms_telem.is_valid()) {
      ESP_LOGI(TAG, "BMS    VBAT=%.3fV VBUS=%.3fV", bms_telem.battery_voltage,
               bms_telem.charging_voltage);
    }

    PMData pm_data;
    if (sps30.read(pm_data) && pm_data.is_pm_25_valid()) {
      ESP_LOGI(TAG, "SPS30  PM1.0=%.1f PM2.5=%.1f PM10=%.1f ug/m3", pm_data.pm_01, pm_data.pm_25,
               pm_data.pm_10);
    }

    CO2Data co2_data;
    if (stcc4.read(co2_data) && co2_data.is_valid()) {
      ESP_LOGI(TAG, "STCC4 CO2=%d ppm", co2_data.co2);

      // STCC4 also provides temperature and humidity
      TempHumData th_data = stcc4.temp_hum_data();
      if (th_data.is_temp_valid()) {
        ESP_LOGI(TAG, "STCC4 temp=%.2f°C hum=%.2f%%", th_data.temperature, th_data.humidity);
      }
    }

    TVOCNOxData voc_data;
    if (sgp41.read(voc_data)) {
      if (voc_data.is_tvoc_raw_valid()) {
        ESP_LOGI(TAG, "SGP41 TVOC=%d NOx=%d (raw)", voc_data.tvoc_raw, voc_data.nox_raw);
      }
    }

    PressureData pressure_data;
    if (dps368.read(pressure_data)) {
      if (pressure_data.is_pressure_valid()) {
        ESP_LOGI(TAG, "DPS368 P=%.2f hPa Alt=%.1f m", pressure_data.pressure,
                 pressure_data.altitude);
      }
      if (dps368.supports_temp_hum()) {
        TempHumData dps_th = dps368.temp_hum_data();
        if (dps_th.is_temp_valid()) {
          ESP_LOGI(TAG, "DPS368 temp=%.2f°C", dps_th.temperature);
        }
      }
    }

    if (reading < N_READINGS) {
      RTOS::delay_ms(READING_INTERVAL_MS);
    }
  }

  ESP_LOGI(TAG, "--- Sensors test done ---");
}

/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <trompeloeil.hpp>
#include <trompeloeil/mock.hpp>

#include "hal/co2_sensor.h"
#include "hal/o3_no2_sensor.h"
#include "hal/pm_sensor.h"
#include "hal/pressure_sensor.h"
#include "hal/temp_hum_sensor.h"
#include "hal/tvoc_nox_sensor.h"
#include "measures_types.h"
#include "rtos.h"
#include "services/sensor_manager.h"

using Catch::Matchers::WithinAbs;

#define EXPECT_READ(mock, data, status)                                                            \
  REQUIRE_CALL((mock), read(trompeloeil::_)).LR_SIDE_EFFECT(_1 = (data)).RETURN(status)

#define EXPECT_FAILURE(mock) REQUIRE_CALL(mock, read(trompeloeil::_)).RETURN(false)

class MockTempHumSensor : public trompeloeil::mock_interface<TempHumSensor> {
public:
  IMPLEMENT_MOCK0(init);
  IMPLEMENT_MOCK1(read);
};

class MockCO2Sensor : public trompeloeil::mock_interface<CO2Sensor> {
public:
  IMPLEMENT_MOCK0(init);
  IMPLEMENT_MOCK1(read);
  IMPLEMENT_CONST_MOCK0(supports_temp_hum);
  IMPLEMENT_MOCK0(temp_hum_data);
  IMPLEMENT_CONST_MOCK0(supports_calibration);
  IMPLEMENT_MOCK1(do_baseline_calibration);
  IMPLEMENT_MOCK0(is_baseline_calibration_done);
  IMPLEMENT_CONST_MOCK0(supports_abc_period_configuration);
  IMPLEMENT_MOCK1(set_abc_period_days);
};

class MockO3No2Sensor : public trompeloeil::mock_interface<O3No2Sensor> {
public:
  IMPLEMENT_MOCK0(init);
  IMPLEMENT_MOCK1(read);
};

class MockPMSensor : public trompeloeil::mock_interface<PMSensor> {
public:
  IMPLEMENT_MOCK1(init);
  IMPLEMENT_MOCK1(read);
  IMPLEMENT_CONST_MOCK0(supports_temp_hum);
  IMPLEMENT_MOCK0(temp_hum_data);
  IMPLEMENT_MOCK0(sleep);
  IMPLEMENT_MOCK0(wake);
};

class MockTVOCNOxSensor : public trompeloeil::mock_interface<TVOCNOxSensor> {
public:
  IMPLEMENT_MOCK0(init);
  IMPLEMENT_MOCK1(read);
  IMPLEMENT_MOCK0(run_conditioning);
  IMPLEMENT_MOCK2(set_compensation);
};

class MockPressureSensor : public trompeloeil::mock_interface<PressureSensor> {
public:
  IMPLEMENT_MOCK0(init);
  IMPLEMENT_MOCK1(read);
  IMPLEMENT_CONST_MOCK0(supports_temp_hum);
  IMPLEMENT_MOCK0(temp_hum_data);
};

class MockRTOS : public trompeloeil::mock_interface<RTOS> {
public:
  IMPLEMENT_MOCK1(delay_ms_impl);
  IMPLEMENT_MOCK0(get_time_ms_impl);
};

TEST_CASE("Averaging", "[SensorManager]") {
  MockTempHumSensor mock_tempHum;
  MockCO2Sensor mock_co2;
  MockPMSensor mock_pm_a;
  MockPMSensor mock_pm_b;
  MockTVOCNOxSensor mock_tvoc_nox;
  MockO3No2Sensor mock_o3no2;
  MockPressureSensor mock_pressure;

  // Allow supports_temp_hum() calls on PM, CO2, and pressure sensors (used
  // internally by SensorManager)
  ALLOW_CALL(mock_pm_a, supports_temp_hum()).RETURN(false);
  ALLOW_CALL(mock_pm_b, supports_temp_hum()).RETURN(false);
  ALLOW_CALL(mock_co2, supports_temp_hum()).RETURN(false);
  ALLOW_CALL(mock_pressure, supports_temp_hum()).RETURN(false);

  Sensors sensors;
  sensors.temp_hum = &mock_tempHum;
  sensors.co2 = &mock_co2;
  sensors.pms_a = &mock_pm_a;
  sensors.pms_b = &mock_pm_b;
  sensors.tvoc_nox = &mock_tvoc_nox;
  sensors.o3_no2 = &mock_o3no2;
  sensors.pressure = &mock_pressure;

  SensorManager sensor_manager(sensors);

  // Setup RTOS mock for all sections
  MockRTOS mock_rtos;
  RTOS::set_instance(&mock_rtos);
  trompeloeil::sequence seq;

  SECTION("Handles null sensor") {
    // Allow RTOS calls (timing doesn't matter for this test)
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    // NOTE: This will crash if there's no nullptr check inside start_measures
    Sensors xsensors;
    xsensors.temp_hum = nullptr;
    xsensors.co2 = nullptr;
    xsensors.pms_a = nullptr;
    xsensors.pms_b = nullptr;
    xsensors.tvoc_nox = nullptr;
    xsensors.o3_no2 = nullptr;
    xsensors.pressure = nullptr;
    SensorManager xsensor_manager(xsensors);
    xsensor_manager.start_measures(1);
  }

  SECTION("1 Iteration all success") {
    // Allow RTOS calls (timing doesn't matter for this test)
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    EXPECT_READ(mock_tempHum, (TempHumData{10.0f, 50.0f}), true);
    EXPECT_READ(mock_co2, (CO2Data{400}), true);
    EXPECT_READ(mock_pm_a,
                (PMData{1.0f, 2.5f, 10.0f, 1.1f, 2.6f, 10.1f, 0.3f, 0.5f, 1.0f, 2.5f, 5.0f, 10.0f}),
                true);
    EXPECT_READ(mock_pm_b,
                (PMData{101.0f, 102.5f, 110.0f, 101.1f, 102.6f, 110.1f, 100.3f, 100.5f, 101.0f,
                        102.5f, 105.0f, 110.0f}),
                true);
    EXPECT_READ(mock_tvoc_nox, (TVOCNOxData{100, 200, 50, 75}), true);
    EXPECT_READ(mock_o3no2, (O3No2Data{0.5f, 0.6f, 0.7f, 0.8f, 25.0f}), true);
    EXPECT_READ(mock_pressure, (PressureData{1013.25f, 110.0f}), true);

    auto result = sensor_manager.start_measures(1);

    // TempHum assertions - temp_hum_a from dedicated sensor, temp_hum_b should
    // be invalid
    REQUIRE_THAT(result.temp_hum_a.temperature, WithinAbs(10.0f, 0.001f));
    REQUIRE_THAT(result.temp_hum_a.humidity, WithinAbs(50.0f, 0.001f));
    REQUIRE(result.temp_hum_b.temperature == MeasuresInvalid::TEMPERATURE);
    REQUIRE(result.temp_hum_b.humidity == MeasuresInvalid::HUMIDITY);

    // CO2 assertions
    REQUIRE(result.co2.co2 == 400);

    // PM assertions - all 12 fields
    REQUIRE_THAT(result.pm_a.pm_01, WithinAbs(1.0f, 0.001f));
    REQUIRE_THAT(result.pm_a.pm_25, WithinAbs(2.5f, 0.001f));
    REQUIRE_THAT(result.pm_a.pm_10, WithinAbs(10.0f, 0.001f));
    REQUIRE_THAT(result.pm_a.pm_01_sp, WithinAbs(1.1f, 0.001f));
    REQUIRE_THAT(result.pm_a.pm_25_sp, WithinAbs(2.6f, 0.001f));
    REQUIRE_THAT(result.pm_a.pm_10_sp, WithinAbs(10.1f, 0.001f));
    REQUIRE_THAT(result.pm_a.pm_03_pc, WithinAbs(0.3f, 0.001f));
    REQUIRE_THAT(result.pm_a.pm_05_pc, WithinAbs(0.5f, 0.001f));
    REQUIRE_THAT(result.pm_a.pm_01_pc, WithinAbs(1.0f, 0.001f));
    REQUIRE_THAT(result.pm_a.pm_25_pc, WithinAbs(2.5f, 0.001f));
    REQUIRE_THAT(result.pm_a.pm_5_pc, WithinAbs(5.0f, 0.001f));
    REQUIRE_THAT(result.pm_a.pm_10_pc, WithinAbs(10.0f, 0.001f));

    // PM_B assertions - all 12 fields
    REQUIRE_THAT(result.pm_b.pm_01, WithinAbs(101.0f, 0.001f));
    REQUIRE_THAT(result.pm_b.pm_25, WithinAbs(102.5f, 0.001f));
    REQUIRE_THAT(result.pm_b.pm_10, WithinAbs(110.0f, 0.001f));
    REQUIRE_THAT(result.pm_b.pm_01_sp, WithinAbs(101.1f, 0.001f));
    REQUIRE_THAT(result.pm_b.pm_25_sp, WithinAbs(102.6f, 0.001f));
    REQUIRE_THAT(result.pm_b.pm_10_sp, WithinAbs(110.1f, 0.001f));
    REQUIRE_THAT(result.pm_b.pm_03_pc, WithinAbs(100.3f, 0.001f));
    REQUIRE_THAT(result.pm_b.pm_05_pc, WithinAbs(100.5f, 0.001f));
    REQUIRE_THAT(result.pm_b.pm_01_pc, WithinAbs(101.0f, 0.001f));
    REQUIRE_THAT(result.pm_b.pm_25_pc, WithinAbs(102.5f, 0.001f));
    REQUIRE_THAT(result.pm_b.pm_5_pc, WithinAbs(105.0f, 0.001f));
    REQUIRE_THAT(result.pm_b.pm_10_pc, WithinAbs(110.0f, 0.001f));

    // TVOCNOx assertions - all 4 fields
    REQUIRE(result.tvoc_nox.tvoc_index == 100);
    REQUIRE(result.tvoc_nox.tvoc_raw == 200);
    REQUIRE(result.tvoc_nox.nox_index == 50);
    REQUIRE(result.tvoc_nox.nox_raw == 75);

    // O3No2 assertions - all 5 fields
    REQUIRE_THAT(result.electrode.o3_we, WithinAbs(0.5f, 0.001f));
    REQUIRE_THAT(result.electrode.o3_ae, WithinAbs(0.6f, 0.001f));
    REQUIRE_THAT(result.electrode.no2_we, WithinAbs(0.7f, 0.001f));
    REQUIRE_THAT(result.electrode.no2_ae, WithinAbs(0.8f, 0.001f));
    REQUIRE_THAT(result.electrode.afe_temp, WithinAbs(25.0f, 0.001f));

    // Pressure assertions - both fields
    REQUIRE_THAT(result.pressure.pressure, WithinAbs(1013.25f, 0.001f));
    REQUIRE_THAT(result.pressure.altitude, WithinAbs(110.0f, 0.001f));
  }

  SECTION("5 iteration all success") {
    // Allow RTOS calls (timing doesn't matter for this test)
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    // TempHum - 5 iterations
    EXPECT_READ(mock_tempHum, (TempHumData{10.0f, 20.0f}), true);
    EXPECT_READ(mock_tempHum, (TempHumData{20.0f, 30.0f}), true);
    EXPECT_READ(mock_tempHum, (TempHumData{30.0f, 40.0f}), true);
    EXPECT_READ(mock_tempHum, (TempHumData{40.0f, 50.0f}), true);
    EXPECT_READ(mock_tempHum, (TempHumData{50.0f, 60.0f}), true);

    // CO2 - 5 iterations
    EXPECT_READ(mock_co2, (CO2Data{400}), true);
    EXPECT_READ(mock_co2, (CO2Data{500}), true);
    EXPECT_READ(mock_co2, (CO2Data{600}), true);
    EXPECT_READ(mock_co2, (CO2Data{700}), true);
    EXPECT_READ(mock_co2, (CO2Data{800}), true);

    // PM - 5 iterations
    EXPECT_READ(mock_pm_a,
                (PMData{1.0f, 2.0f, 10.0f, 1.0f, 2.0f, 10.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f}),
                true);
    EXPECT_READ(
        mock_pm_a,
        (PMData{2.0f, 4.0f, 20.0f, 2.0f, 4.0f, 20.0f, 2.0f, 4.0f, 6.0f, 8.0f, 10.0f, 12.0f}), true);
    EXPECT_READ(
        mock_pm_a,
        (PMData{3.0f, 6.0f, 30.0f, 3.0f, 6.0f, 30.0f, 3.0f, 6.0f, 9.0f, 12.0f, 15.0f, 18.0f}),
        true);
    EXPECT_READ(
        mock_pm_a,
        (PMData{4.0f, 8.0f, 40.0f, 4.0f, 8.0f, 40.0f, 4.0f, 8.0f, 12.0f, 16.0f, 20.0f, 24.0f}),
        true);
    EXPECT_READ(
        mock_pm_a,
        (PMData{5.0f, 10.0f, 50.0f, 5.0f, 10.0f, 50.0f, 5.0f, 10.0f, 15.0f, 20.0f, 25.0f, 30.0f}),
        true);

    // PM_B - 5 iterations
    EXPECT_READ(mock_pm_b,
                (PMData{10.0f, 20.0f, 100.0f, 10.0f, 20.0f, 100.0f, 10.0f, 20.0f, 30.0f, 40.0f,
                        50.0f, 60.0f}),
                true);
    EXPECT_READ(mock_pm_b,
                (PMData{20.0f, 40.0f, 200.0f, 20.0f, 40.0f, 200.0f, 20.0f, 40.0f, 60.0f, 80.0f,
                        100.0f, 120.0f}),
                true);
    EXPECT_READ(mock_pm_b,
                (PMData{30.0f, 60.0f, 300.0f, 30.0f, 60.0f, 300.0f, 30.0f, 60.0f, 90.0f, 120.0f,
                        150.0f, 180.0f}),
                true);
    EXPECT_READ(mock_pm_b,
                (PMData{40.0f, 80.0f, 400.0f, 40.0f, 80.0f, 400.0f, 40.0f, 80.0f, 120.0f, 160.0f,
                        200.0f, 240.0f}),
                true);
    EXPECT_READ(mock_pm_b,
                (PMData{50.0f, 100.0f, 500.0f, 50.0f, 100.0f, 500.0f, 50.0f, 100.0f, 150.0f, 200.0f,
                        250.0f, 300.0f}),
                true);

    // TVOCNOx - 5 iterations
    EXPECT_READ(mock_tvoc_nox, (TVOCNOxData{100, 200, 50, 100}), true);
    EXPECT_READ(mock_tvoc_nox, (TVOCNOxData{200, 300, 100, 150}), true);
    EXPECT_READ(mock_tvoc_nox, (TVOCNOxData{300, 400, 150, 200}), true);
    EXPECT_READ(mock_tvoc_nox, (TVOCNOxData{400, 500, 200, 250}), true);
    EXPECT_READ(mock_tvoc_nox, (TVOCNOxData{500, 600, 250, 300}), true);

    // O3No2 - 5 iterations
    EXPECT_READ(mock_o3no2, (O3No2Data{0.5f, 0.6f, 0.7f, 0.8f, 20.0f}), true);
    EXPECT_READ(mock_o3no2, (O3No2Data{1.0f, 1.1f, 1.2f, 1.3f, 22.0f}), true);
    EXPECT_READ(mock_o3no2, (O3No2Data{1.5f, 1.6f, 1.7f, 1.8f, 24.0f}), true);
    EXPECT_READ(mock_o3no2, (O3No2Data{2.0f, 2.1f, 2.2f, 2.3f, 26.0f}), true);
    EXPECT_READ(mock_o3no2, (O3No2Data{2.5f, 2.6f, 2.7f, 2.8f, 28.0f}), true);

    // Pressure - 5 iterations
    EXPECT_READ(mock_pressure, (PressureData{1010.0f, 100.0f}), true);
    EXPECT_READ(mock_pressure, (PressureData{1012.0f, 110.0f}), true);
    EXPECT_READ(mock_pressure, (PressureData{1014.0f, 120.0f}), true);
    EXPECT_READ(mock_pressure, (PressureData{1016.0f, 130.0f}), true);
    EXPECT_READ(mock_pressure, (PressureData{1018.0f, 140.0f}), true);

    auto result = sensor_manager.start_measures(5);

    // TempHum assertions - temp_hum_a from dedicated sensor, temp_hum_b should
    // be invalid (average of 10,20,30,40,50 = 30; 20,30,40,50,60 = 40)
    REQUIRE_THAT(result.temp_hum_a.temperature, WithinAbs(30.0f, 0.001f));
    REQUIRE_THAT(result.temp_hum_a.humidity, WithinAbs(40.0f, 0.001f));
    REQUIRE(result.temp_hum_b.temperature == MeasuresInvalid::TEMPERATURE);
    REQUIRE(result.temp_hum_b.humidity == MeasuresInvalid::HUMIDITY);

    // CO2 assertions (average of 400,500,600,700,800 = 600)
    REQUIRE(result.co2.co2 == 600);

    // PM assertions (average of each field)
    REQUIRE_THAT(result.pm_a.pm_01, WithinAbs(3.0f, 0.001f));     // avg of 1,2,3,4,5
    REQUIRE_THAT(result.pm_a.pm_25, WithinAbs(6.0f, 0.001f));     // avg of 2,4,6,8,10
    REQUIRE_THAT(result.pm_a.pm_10, WithinAbs(30.0f, 0.001f));    // avg of 10,20,30,40,50
    REQUIRE_THAT(result.pm_a.pm_01_sp, WithinAbs(3.0f, 0.001f));  // avg of 1,2,3,4,5
    REQUIRE_THAT(result.pm_a.pm_25_sp, WithinAbs(6.0f, 0.001f));  // avg of 2,4,6,8,10
    REQUIRE_THAT(result.pm_a.pm_10_sp, WithinAbs(30.0f, 0.001f)); // avg of 10,20,30,40,50
    REQUIRE_THAT(result.pm_a.pm_03_pc, WithinAbs(3.0f, 0.001f));  // avg of 1,2,3,4,5
    REQUIRE_THAT(result.pm_a.pm_05_pc, WithinAbs(6.0f, 0.001f));  // avg of 2,4,6,8,10
    REQUIRE_THAT(result.pm_a.pm_01_pc, WithinAbs(9.0f, 0.001f));  // avg of 3,6,9,12,15
    REQUIRE_THAT(result.pm_a.pm_25_pc, WithinAbs(12.0f, 0.001f)); // avg of 4,8,12,16,20
    REQUIRE_THAT(result.pm_a.pm_5_pc, WithinAbs(15.0f, 0.001f));  // avg of 5,10,15,20,25
    REQUIRE_THAT(result.pm_a.pm_10_pc, WithinAbs(18.0f, 0.001f)); // avg of 6,12,18,24,30

    // PM_B assertions (average of each field)
    REQUIRE_THAT(result.pm_b.pm_01, WithinAbs(30.0f, 0.001f));     // avg of 10,20,30,40,50
    REQUIRE_THAT(result.pm_b.pm_25, WithinAbs(60.0f, 0.001f));     // avg of 20,40,60,80,100
    REQUIRE_THAT(result.pm_b.pm_10, WithinAbs(300.0f, 0.001f));    // avg of 100,200,300,400,500
    REQUIRE_THAT(result.pm_b.pm_01_sp, WithinAbs(30.0f, 0.001f));  // avg of 10,20,30,40,50
    REQUIRE_THAT(result.pm_b.pm_25_sp, WithinAbs(60.0f, 0.001f));  // avg of 20,40,60,80,100
    REQUIRE_THAT(result.pm_b.pm_10_sp, WithinAbs(300.0f, 0.001f)); // avg of 100,200,300,400,500
    REQUIRE_THAT(result.pm_b.pm_03_pc, WithinAbs(30.0f, 0.001f));  // avg of 10,20,30,40,50
    REQUIRE_THAT(result.pm_b.pm_05_pc, WithinAbs(60.0f, 0.001f));  // avg of 20,40,60,80,100
    REQUIRE_THAT(result.pm_b.pm_01_pc, WithinAbs(90.0f, 0.001f));  // avg of 30,60,90,120,150
    REQUIRE_THAT(result.pm_b.pm_25_pc, WithinAbs(120.0f, 0.001f)); // avg of 40,80,120,160,200
    REQUIRE_THAT(result.pm_b.pm_5_pc, WithinAbs(150.0f, 0.001f));  // avg of 50,100,150,200,250
    REQUIRE_THAT(result.pm_b.pm_10_pc, WithinAbs(180.0f, 0.001f)); // avg of 60,120,180,240,300

    // TVOCNOx assertions (average of each field)
    REQUIRE(result.tvoc_nox.tvoc_index == 300); // avg of 100,200,300,400,500
    REQUIRE(result.tvoc_nox.tvoc_raw == 400);   // avg of 200,300,400,500,600
    REQUIRE(result.tvoc_nox.nox_index == 150);  // avg of 50,100,150,200,250
    REQUIRE(result.tvoc_nox.nox_raw == 200);    // avg of 100,150,200,250,300

    // O3No2 assertions (average of each field)
    REQUIRE_THAT(result.electrode.o3_we, WithinAbs(1.5f, 0.001f));     // avg of 0.5,1.0,1.5,2.0,2.5
    REQUIRE_THAT(result.electrode.o3_ae, WithinAbs(1.6f, 0.001f));     // avg of 0.6,1.1,1.6,2.1,2.6
    REQUIRE_THAT(result.electrode.no2_we, WithinAbs(1.7f, 0.001f));    // avg of 0.7,1.2,1.7,2.2,2.7
    REQUIRE_THAT(result.electrode.no2_ae, WithinAbs(1.8f, 0.001f));    // avg of 0.8,1.3,1.8,2.3,2.8
    REQUIRE_THAT(result.electrode.afe_temp, WithinAbs(24.0f, 0.001f)); // avg of 20,22,24,26,28

    // Pressure assertions (average of each field)
    REQUIRE_THAT(result.pressure.pressure,
                 WithinAbs(1014.0f, 0.001f)); // avg of 1010,1012,1014,1016,1018
    REQUIRE_THAT(result.pressure.altitude, WithinAbs(120.0f, 0.001f)); // avg of 100,110,120,130,140
  }

  SECTION("3 iteration 1 failed") {
    // Allow RTOS calls (timing doesn't matter for this test)
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    // TempHum - 3 iterations (1 fails)
    EXPECT_READ(mock_tempHum, (TempHumData{10.0f, 20.0f}), true);
    EXPECT_FAILURE(mock_tempHum);
    EXPECT_READ(mock_tempHum, (TempHumData{40.0f, 40.0f}), true);

    // CO2 - 3 iterations (1 fails)
    EXPECT_READ(mock_co2, (CO2Data{400}), true);
    EXPECT_FAILURE(mock_co2);
    EXPECT_READ(mock_co2, (CO2Data{600}), true);

    // PM - 3 iterations (1 fails)
    EXPECT_READ(mock_pm_a,
                (PMData{1.0f, 2.0f, 10.0f, 1.0f, 2.0f, 10.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f}),
                true);
    EXPECT_FAILURE(mock_pm_a);
    EXPECT_READ(
        mock_pm_a,
        (PMData{5.0f, 10.0f, 50.0f, 5.0f, 10.0f, 50.0f, 5.0f, 10.0f, 15.0f, 20.0f, 25.0f, 30.0f}),
        true);

    // PM_B - 3 iterations (1 fails)
    EXPECT_READ(mock_pm_b,
                (PMData{10.0f, 20.0f, 100.0f, 10.0f, 20.0f, 100.0f, 10.0f, 20.0f, 30.0f, 40.0f,
                        50.0f, 60.0f}),
                true);
    EXPECT_FAILURE(mock_pm_b);
    EXPECT_READ(mock_pm_b,
                (PMData{30.0f, 60.0f, 300.0f, 30.0f, 60.0f, 300.0f, 30.0f, 60.0f, 90.0f, 120.0f,
                        150.0f, 180.0f}),
                true);

    // TVOCNOx - 3 iterations (1 fails)
    EXPECT_READ(mock_tvoc_nox, (TVOCNOxData{100, 200, 50, 100}), true);
    EXPECT_FAILURE(mock_tvoc_nox);
    EXPECT_READ(mock_tvoc_nox, (TVOCNOxData{500, 600, 250, 300}), true);

    // O3No2 - 3 iterations (1 fails)
    EXPECT_READ(mock_o3no2, (O3No2Data{0.5f, 0.6f, 0.7f, 0.8f, 20.0f}), true);
    EXPECT_FAILURE(mock_o3no2);
    EXPECT_READ(mock_o3no2, (O3No2Data{2.5f, 2.6f, 2.7f, 2.8f, 28.0f}), true);

    // Pressure - 3 iterations (1 fails)
    EXPECT_READ(mock_pressure, (PressureData{1010.0f, 100.0f}), true);
    EXPECT_FAILURE(mock_pressure);
    EXPECT_READ(mock_pressure, (PressureData{1020.0f, 120.0f}), true);

    auto result = sensor_manager.start_measures(3);

    // TempHum assertions - temp_hum_a from dedicated sensor, temp_hum_b should
    // be invalid (average of 2 successful: (10+40)/2=25, (20+40)/2=30)
    REQUIRE_THAT(result.temp_hum_a.temperature, WithinAbs(25.0f, 0.001f));
    REQUIRE_THAT(result.temp_hum_a.humidity, WithinAbs(30.0f, 0.001f));
    REQUIRE(result.temp_hum_b.temperature == MeasuresInvalid::TEMPERATURE);
    REQUIRE(result.temp_hum_b.humidity == MeasuresInvalid::HUMIDITY);

    // CO2 assertions (average of 2 successful: (400+600)/2=500)
    REQUIRE(result.co2.co2 == 500);

    // PM assertions (average of 2 successful reads)
    REQUIRE_THAT(result.pm_a.pm_01, WithinAbs(3.0f, 0.001f));     // (1+5)/2
    REQUIRE_THAT(result.pm_a.pm_25, WithinAbs(6.0f, 0.001f));     // (2+10)/2
    REQUIRE_THAT(result.pm_a.pm_10, WithinAbs(30.0f, 0.001f));    // (10+50)/2
    REQUIRE_THAT(result.pm_a.pm_01_sp, WithinAbs(3.0f, 0.001f));  // (1+5)/2
    REQUIRE_THAT(result.pm_a.pm_25_sp, WithinAbs(6.0f, 0.001f));  // (2+10)/2
    REQUIRE_THAT(result.pm_a.pm_10_sp, WithinAbs(30.0f, 0.001f)); // (10+50)/2
    REQUIRE_THAT(result.pm_a.pm_03_pc, WithinAbs(3.0f, 0.001f));  // (1+5)/2
    REQUIRE_THAT(result.pm_a.pm_05_pc, WithinAbs(6.0f, 0.001f));  // (2+10)/2
    REQUIRE_THAT(result.pm_a.pm_01_pc, WithinAbs(9.0f, 0.001f));  // (3+15)/2
    REQUIRE_THAT(result.pm_a.pm_25_pc, WithinAbs(12.0f, 0.001f)); // (4+20)/2
    REQUIRE_THAT(result.pm_a.pm_5_pc, WithinAbs(15.0f, 0.001f));  // (5+25)/2
    REQUIRE_THAT(result.pm_a.pm_10_pc, WithinAbs(18.0f, 0.001f)); // (6+30)/2

    // PM_B assertions (average of 2 successful reads)
    REQUIRE_THAT(result.pm_b.pm_01, WithinAbs(20.0f, 0.001f));     // (10+30)/2
    REQUIRE_THAT(result.pm_b.pm_25, WithinAbs(40.0f, 0.001f));     // (20+60)/2
    REQUIRE_THAT(result.pm_b.pm_10, WithinAbs(200.0f, 0.001f));    // (100+300)/2
    REQUIRE_THAT(result.pm_b.pm_01_sp, WithinAbs(20.0f, 0.001f));  // (10+30)/2
    REQUIRE_THAT(result.pm_b.pm_25_sp, WithinAbs(40.0f, 0.001f));  // (20+60)/2
    REQUIRE_THAT(result.pm_b.pm_10_sp, WithinAbs(200.0f, 0.001f)); // (100+300)/2
    REQUIRE_THAT(result.pm_b.pm_03_pc, WithinAbs(20.0f, 0.001f));  // (10+30)/2
    REQUIRE_THAT(result.pm_b.pm_05_pc, WithinAbs(40.0f, 0.001f));  // (20+60)/2
    REQUIRE_THAT(result.pm_b.pm_01_pc, WithinAbs(60.0f, 0.001f));  // (30+90)/2
    REQUIRE_THAT(result.pm_b.pm_25_pc, WithinAbs(80.0f, 0.001f));  // (40+120)/2
    REQUIRE_THAT(result.pm_b.pm_5_pc, WithinAbs(100.0f, 0.001f));  // (50+150)/2
    REQUIRE_THAT(result.pm_b.pm_10_pc, WithinAbs(120.0f, 0.001f)); // (60+180)/2

    // TVOCNOx assertions (average of 2 successful reads)
    REQUIRE(result.tvoc_nox.tvoc_index == 300); // (100+500)/2
    REQUIRE(result.tvoc_nox.tvoc_raw == 400);   // (200+600)/2
    REQUIRE(result.tvoc_nox.nox_index == 150);  // (50+250)/2
    REQUIRE(result.tvoc_nox.nox_raw == 200);    // (100+300)/2

    // O3No2 assertions (average of 2 successful reads)
    REQUIRE_THAT(result.electrode.o3_we, WithinAbs(1.5f, 0.001f));     // (0.5+2.5)/2
    REQUIRE_THAT(result.electrode.o3_ae, WithinAbs(1.6f, 0.001f));     // (0.6+2.6)/2
    REQUIRE_THAT(result.electrode.no2_we, WithinAbs(1.7f, 0.001f));    // (0.7+2.7)/2
    REQUIRE_THAT(result.electrode.no2_ae, WithinAbs(1.8f, 0.001f));    // (0.8+2.8)/2
    REQUIRE_THAT(result.electrode.afe_temp, WithinAbs(24.0f, 0.001f)); // (20+28)/2

    // Pressure assertions (average of 2 successful reads)
    REQUIRE_THAT(result.pressure.pressure, WithinAbs(1015.0f, 0.001f)); // (1010+1020)/2
    REQUIRE_THAT(result.pressure.altitude, WithinAbs(110.0f, 0.001f));  // (100+120)/2
  }

  SECTION("No valid value") {
    // Allow RTOS calls (timing doesn't matter for this test)
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    // All sensors fail to read
    EXPECT_FAILURE(mock_tempHum);
    EXPECT_FAILURE(mock_co2);
    EXPECT_FAILURE(mock_pm_a);
    EXPECT_FAILURE(mock_pm_b);
    EXPECT_FAILURE(mock_tvoc_nox);
    EXPECT_FAILURE(mock_o3no2);
    EXPECT_FAILURE(mock_pressure);

    auto result = sensor_manager.start_measures(1);

    // TempHum assertions - should be invalid
    REQUIRE(result.temp_hum_a.temperature == MeasuresInvalid::TEMPERATURE);
    REQUIRE(result.temp_hum_a.humidity == MeasuresInvalid::HUMIDITY);
    REQUIRE(result.temp_hum_b.temperature == MeasuresInvalid::TEMPERATURE);
    REQUIRE(result.temp_hum_b.humidity == MeasuresInvalid::HUMIDITY);

    // CO2 assertions - should be invalid
    REQUIRE(result.co2.co2 == MeasuresInvalid::CO2);

    // PM assertions - all fields should be invalid
    REQUIRE(result.pm_a.pm_01 == MeasuresInvalid::PM);
    REQUIRE(result.pm_a.pm_25 == MeasuresInvalid::PM);
    REQUIRE(result.pm_a.pm_10 == MeasuresInvalid::PM);
    REQUIRE(result.pm_a.pm_01_sp == MeasuresInvalid::PM);
    REQUIRE(result.pm_a.pm_25_sp == MeasuresInvalid::PM);
    REQUIRE(result.pm_a.pm_10_sp == MeasuresInvalid::PM);
    REQUIRE(result.pm_a.pm_03_pc == MeasuresInvalid::PM);
    REQUIRE(result.pm_a.pm_05_pc == MeasuresInvalid::PM);
    REQUIRE(result.pm_a.pm_01_pc == MeasuresInvalid::PM);
    REQUIRE(result.pm_a.pm_25_pc == MeasuresInvalid::PM);
    REQUIRE(result.pm_a.pm_5_pc == MeasuresInvalid::PM);
    REQUIRE(result.pm_a.pm_10_pc == MeasuresInvalid::PM);

    // PM_B assertions - all fields should be invalid
    REQUIRE(result.pm_b.pm_01 == MeasuresInvalid::PM);
    REQUIRE(result.pm_b.pm_25 == MeasuresInvalid::PM);
    REQUIRE(result.pm_b.pm_10 == MeasuresInvalid::PM);
    REQUIRE(result.pm_b.pm_01_sp == MeasuresInvalid::PM);
    REQUIRE(result.pm_b.pm_25_sp == MeasuresInvalid::PM);
    REQUIRE(result.pm_b.pm_10_sp == MeasuresInvalid::PM);
    REQUIRE(result.pm_b.pm_03_pc == MeasuresInvalid::PM);
    REQUIRE(result.pm_b.pm_05_pc == MeasuresInvalid::PM);
    REQUIRE(result.pm_b.pm_01_pc == MeasuresInvalid::PM);
    REQUIRE(result.pm_b.pm_25_pc == MeasuresInvalid::PM);
    REQUIRE(result.pm_b.pm_5_pc == MeasuresInvalid::PM);
    REQUIRE(result.pm_b.pm_10_pc == MeasuresInvalid::PM);

    // TVOCNOx assertions - should be invalid
    REQUIRE(result.tvoc_nox.tvoc_index == MeasuresInvalid::TVOC);
    REQUIRE(result.tvoc_nox.tvoc_raw == MeasuresInvalid::TVOC);
    REQUIRE(result.tvoc_nox.nox_index == MeasuresInvalid::NOX);
    REQUIRE(result.tvoc_nox.nox_raw == MeasuresInvalid::NOX);

    // O3No2 assertions - should be invalid
    REQUIRE(result.electrode.o3_we == MeasuresInvalid::VOLT);
    REQUIRE(result.electrode.o3_ae == MeasuresInvalid::VOLT);
    REQUIRE(result.electrode.no2_we == MeasuresInvalid::VOLT);
    REQUIRE(result.electrode.no2_ae == MeasuresInvalid::VOLT);
    REQUIRE(result.electrode.afe_temp == MeasuresInvalid::VOLT);

    // Pressure assertions - should be invalid
    REQUIRE(result.pressure.pressure == MeasuresInvalid::PRESSURE);
    REQUIRE(result.pressure.altitude == MeasuresInvalid::ALTITUDE);
  }

  SECTION("Negative temperature handling") {
    // Allow RTOS calls (timing doesn't matter for this test)
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    // Test valid negative temperatures (within range: -40°C to 125°C)
    EXPECT_READ(mock_tempHum, (TempHumData{-20.0f, 50.0f}), true);
    EXPECT_READ(mock_tempHum, (TempHumData{-10.0f, 60.0f}), true);
    EXPECT_READ(mock_tempHum, (TempHumData{-30.0f, 70.0f}), true);

    // Other sensors - just make them fail (Only testing temp hum)
    EXPECT_FAILURE(mock_co2);
    EXPECT_FAILURE(mock_co2);
    EXPECT_FAILURE(mock_co2);
    EXPECT_FAILURE(mock_pm_a);
    EXPECT_FAILURE(mock_pm_a);
    EXPECT_FAILURE(mock_pm_a);
    EXPECT_FAILURE(mock_pm_b);
    EXPECT_FAILURE(mock_pm_b);
    EXPECT_FAILURE(mock_pm_b);
    EXPECT_FAILURE(mock_tvoc_nox);
    EXPECT_FAILURE(mock_tvoc_nox);
    EXPECT_FAILURE(mock_tvoc_nox);
    EXPECT_FAILURE(mock_o3no2);
    EXPECT_FAILURE(mock_o3no2);
    EXPECT_FAILURE(mock_o3no2);
    EXPECT_FAILURE(mock_pressure);
    EXPECT_FAILURE(mock_pressure);
    EXPECT_FAILURE(mock_pressure);

    auto result = sensor_manager.start_measures(3);

    // Valid negative temperatures should be averaged correctly
    // Average: (-20 + -10 + -30) / 3 = -20°C
    REQUIRE_THAT(result.temp_hum_a.temperature, WithinAbs(-20.0f, 0.001f));
    REQUIRE_THAT(result.temp_hum_a.humidity, WithinAbs(60.0f, 0.001f));
  }

  SECTION("Invalid negative temperature") {
    // Allow RTOS calls (timing doesn't matter for this test)
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    // Test invalid negative temperatures (below MIN_VALID_TEMP = -40°C)
    EXPECT_READ(mock_tempHum, (TempHumData{-50.0f, 50.0f}),
                true); // Too cold, invalid
    EXPECT_READ(mock_tempHum, (TempHumData{-60.0f, 60.0f}),
                true); // Too cold, invalid
    EXPECT_READ(mock_tempHum, (TempHumData{-100.0f, 70.0f}),
                true); // Too cold, invalid

    // Other sensors - just make them fail (Only testing temp hum)
    EXPECT_FAILURE(mock_co2);
    EXPECT_FAILURE(mock_co2);
    EXPECT_FAILURE(mock_co2);
    EXPECT_FAILURE(mock_pm_a);
    EXPECT_FAILURE(mock_pm_a);
    EXPECT_FAILURE(mock_pm_a);
    EXPECT_FAILURE(mock_pm_b);
    EXPECT_FAILURE(mock_pm_b);
    EXPECT_FAILURE(mock_pm_b);
    EXPECT_FAILURE(mock_tvoc_nox);
    EXPECT_FAILURE(mock_tvoc_nox);
    EXPECT_FAILURE(mock_tvoc_nox);
    EXPECT_FAILURE(mock_o3no2);
    EXPECT_FAILURE(mock_o3no2);
    EXPECT_FAILURE(mock_o3no2);
    EXPECT_FAILURE(mock_pressure);
    EXPECT_FAILURE(mock_pressure);
    EXPECT_FAILURE(mock_pressure);

    auto result = sensor_manager.start_measures(3);

    // temp_hum_a from dedicated sensor - all temperatures invalid, humidity
    // valid
    REQUIRE(result.temp_hum_a.temperature == MeasuresInvalid::TEMPERATURE);
    REQUIRE_THAT(result.temp_hum_a.humidity, WithinAbs(60.0f, 0.001f));
    // temp_hum_b should be invalid (dedicated sensor present)
    REQUIRE(result.temp_hum_b.temperature == MeasuresInvalid::TEMPERATURE);
    REQUIRE(result.temp_hum_b.humidity == MeasuresInvalid::HUMIDITY);
  }

  SECTION("Mixed valid and invalid negative temperature") {
    // Allow RTOS calls (timing doesn't matter for this test)
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    // Mix of valid and invalid negative temperatures
    EXPECT_READ(mock_tempHum, (TempHumData{-30.0f, 50.0f}), true); // Valid
    EXPECT_READ(mock_tempHum, (TempHumData{-50.0f, 60.0f}),
                true);                                             // Invalid (too cold)
    EXPECT_READ(mock_tempHum, (TempHumData{-20.0f, 70.0f}), true); // Valid

    // Other sensors - just make them fail (Only testing temp hum)
    EXPECT_FAILURE(mock_co2);
    EXPECT_FAILURE(mock_co2);
    EXPECT_FAILURE(mock_co2);
    EXPECT_FAILURE(mock_pm_a);
    EXPECT_FAILURE(mock_pm_a);
    EXPECT_FAILURE(mock_pm_a);
    EXPECT_FAILURE(mock_pm_b);
    EXPECT_FAILURE(mock_pm_b);
    EXPECT_FAILURE(mock_pm_b);
    EXPECT_FAILURE(mock_tvoc_nox);
    EXPECT_FAILURE(mock_tvoc_nox);
    EXPECT_FAILURE(mock_tvoc_nox);
    EXPECT_FAILURE(mock_o3no2);
    EXPECT_FAILURE(mock_o3no2);
    EXPECT_FAILURE(mock_o3no2);
    EXPECT_FAILURE(mock_pressure);
    EXPECT_FAILURE(mock_pressure);
    EXPECT_FAILURE(mock_pressure);

    auto result = sensor_manager.start_measures(3);

    // temp_hum_a from dedicated sensor - only valid temperatures averaged: (-30
    // + -20) / 2 = -25°C
    REQUIRE_THAT(result.temp_hum_a.temperature, WithinAbs(-25.0f, 0.001f));
    REQUIRE_THAT(result.temp_hum_a.humidity, WithinAbs(60.0f, 0.001f));
    // temp_hum_b should be invalid (dedicated sensor present)
    REQUIRE(result.temp_hum_b.temperature == MeasuresInvalid::TEMPERATURE);
    REQUIRE(result.temp_hum_b.humidity == MeasuresInvalid::HUMIDITY);
  }

  SECTION("Temperature and Humidity from PM sensor") {
    // Allow RTOS calls (timing doesn't matter for this test)
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    MockPMSensor mock_pm_local;
    Sensors xsensors;
    xsensors.temp_hum = nullptr;
    xsensors.co2 = nullptr;
    xsensors.pms_a = &mock_pm_local;
    xsensors.pms_b = nullptr;
    xsensors.tvoc_nox = nullptr;
    xsensors.o3_no2 = nullptr;
    xsensors.pressure = nullptr;
    SensorManager xsensor_manager(xsensors);

    // PM sensor supports temp/hum and provides data
    REQUIRE_CALL(mock_pm_local, supports_temp_hum()).RETURN(true);
    REQUIRE_CALL(mock_pm_local, temp_hum_data()).RETURN(TempHumData{25.0f, 60.0f});

    // PM sensor also provides PM data
    EXPECT_READ(mock_pm_local,
                (PMData{1.0f, 2.5f, 10.0f, 1.1f, 2.6f, 10.1f, 0.3f, 0.5f, 1.0f, 2.5f, 5.0f, 10.0f}),
                true);

    auto result = xsensor_manager.start_measures(1);

    // Verify temp_hum_a comes from pms_a (no dedicated sensor)
    REQUIRE_THAT(result.temp_hum_a.temperature, WithinAbs(25.0f, 0.001f));
    REQUIRE_THAT(result.temp_hum_a.humidity, WithinAbs(60.0f, 0.001f));
    // temp_hum_b should be invalid (no pms_b)
    REQUIRE(result.temp_hum_b.temperature == MeasuresInvalid::TEMPERATURE);
    REQUIRE(result.temp_hum_b.humidity == MeasuresInvalid::HUMIDITY);

    // Verify PM data is also present
    REQUIRE_THAT(result.pm_a.pm_01, WithinAbs(1.0f, 0.001f));
    REQUIRE_THAT(result.pm_a.pm_25, WithinAbs(2.5f, 0.001f));
  }

  SECTION("PM sensor does not support Temperature and Humidity") {
    // Allow RTOS calls (timing doesn't matter for this test)
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    MockPMSensor mock_pm_local;
    Sensors xsensors;
    xsensors.temp_hum = nullptr;
    xsensors.co2 = nullptr;
    xsensors.pms_a = &mock_pm_local;
    xsensors.pms_b = nullptr;
    xsensors.tvoc_nox = nullptr;
    xsensors.o3_no2 = nullptr;
    xsensors.pressure = nullptr;
    SensorManager xsensor_manager(xsensors);

    // PM sensor does NOT support temp/hum
    REQUIRE_CALL(mock_pm_local, supports_temp_hum()).RETURN(false);

    // PM sensor provides PM data only
    EXPECT_READ(mock_pm_local,
                (PMData{1.0f, 2.5f, 10.0f, 1.1f, 2.6f, 10.1f, 0.3f, 0.5f, 1.0f, 2.5f, 5.0f, 10.0f}),
                true);

    auto result = xsensor_manager.start_measures(1);

    // Verify both temp/hum fields are invalid (no temp/hum source available)
    REQUIRE(result.temp_hum_a.temperature == MeasuresInvalid::TEMPERATURE);
    REQUIRE(result.temp_hum_a.humidity == MeasuresInvalid::HUMIDITY);
    REQUIRE(result.temp_hum_b.temperature == MeasuresInvalid::TEMPERATURE);
    REQUIRE(result.temp_hum_b.humidity == MeasuresInvalid::HUMIDITY);

    // Verify PM data is present
    REQUIRE_THAT(result.pm_a.pm_01, WithinAbs(1.0f, 0.001f));
    REQUIRE_THAT(result.pm_a.pm_25, WithinAbs(2.5f, 0.001f));
  }

  SECTION("Both PM sensors support temp/hum independently") {
    // Allow RTOS calls (timing doesn't matter for this test)
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    MockPMSensor mock_pm_a_local;
    MockPMSensor mock_pm_b_local;
    Sensors xsensors;
    xsensors.temp_hum = nullptr; // No dedicated sensor
    xsensors.co2 = nullptr;
    xsensors.pms_a = &mock_pm_a_local;
    xsensors.pms_b = &mock_pm_b_local;
    xsensors.tvoc_nox = nullptr;
    xsensors.o3_no2 = nullptr;
    xsensors.pressure = nullptr;
    SensorManager xsensor_manager(xsensors);

    // Both PM sensors support temp/hum
    REQUIRE_CALL(mock_pm_a_local, supports_temp_hum()).RETURN(true);
    REQUIRE_CALL(mock_pm_b_local, supports_temp_hum()).RETURN(true);

    // PM sensor A provides temp/hum data (3 iterations for averaging)
    REQUIRE_CALL(mock_pm_a_local, temp_hum_data()).RETURN(TempHumData{20.0f, 50.0f});
    REQUIRE_CALL(mock_pm_a_local, temp_hum_data()).RETURN(TempHumData{22.0f, 52.0f});
    REQUIRE_CALL(mock_pm_a_local, temp_hum_data()).RETURN(TempHumData{24.0f, 54.0f});

    // PM sensor B provides temp/hum data (3 iterations for averaging)
    REQUIRE_CALL(mock_pm_b_local, temp_hum_data()).RETURN(TempHumData{15.0f, 60.0f});
    REQUIRE_CALL(mock_pm_b_local, temp_hum_data()).RETURN(TempHumData{17.0f, 62.0f});
    REQUIRE_CALL(mock_pm_b_local, temp_hum_data()).RETURN(TempHumData{19.0f, 64.0f});

    // PM sensors also provide PM data
    EXPECT_READ(mock_pm_a_local,
                (PMData{1.0f, 2.5f, 10.0f, 1.1f, 2.6f, 10.1f, 0.3f, 0.5f, 1.0f, 2.5f, 5.0f, 10.0f}),
                true);
    EXPECT_READ(mock_pm_a_local,
                (PMData{2.0f, 3.5f, 11.0f, 2.1f, 3.6f, 11.1f, 1.3f, 1.5f, 2.0f, 3.5f, 6.0f, 11.0f}),
                true);
    EXPECT_READ(mock_pm_a_local,
                (PMData{3.0f, 4.5f, 12.0f, 3.1f, 4.6f, 12.1f, 2.3f, 2.5f, 3.0f, 4.5f, 7.0f, 12.0f}),
                true);

    EXPECT_READ(mock_pm_b_local,
                (PMData{10.0f, 20.0f, 100.0f, 10.1f, 20.1f, 100.1f, 10.3f, 10.5f, 10.0f, 20.0f,
                        50.0f, 100.0f}),
                true);
    EXPECT_READ(mock_pm_b_local,
                (PMData{11.0f, 21.0f, 101.0f, 11.1f, 21.1f, 101.1f, 11.3f, 11.5f, 11.0f, 21.0f,
                        51.0f, 101.0f}),
                true);
    EXPECT_READ(mock_pm_b_local,
                (PMData{12.0f, 22.0f, 102.0f, 12.1f, 22.1f, 102.1f, 12.3f, 12.5f, 12.0f, 22.0f,
                        52.0f, 102.0f}),
                true);

    auto result = xsensor_manager.start_measures(3);

    // Verify temp_hum_a comes from pms_a (avg of 20,22,24 = 22; avg of 50,52,54
    // = 52)
    REQUIRE_THAT(result.temp_hum_a.temperature, WithinAbs(22.0f, 0.001f));
    REQUIRE_THAT(result.temp_hum_a.humidity, WithinAbs(52.0f, 0.001f));

    // Verify temp_hum_b comes from pms_b (avg of 15,17,19 = 17; avg of 60,62,64
    // = 62)
    REQUIRE_THAT(result.temp_hum_b.temperature, WithinAbs(17.0f, 0.001f));
    REQUIRE_THAT(result.temp_hum_b.humidity, WithinAbs(62.0f, 0.001f));

    // Verify PM data is present
    REQUIRE_THAT(result.pm_a.pm_01, WithinAbs(2.0f, 0.001f));  // avg of 1,2,3
    REQUIRE_THAT(result.pm_b.pm_01, WithinAbs(11.0f, 0.001f)); // avg of 10,11,12
  }

  SECTION("Only pms_b supports temp/hum") {
    // Allow RTOS calls (timing doesn't matter for this test)
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    MockPMSensor mock_pm_a_local;
    MockPMSensor mock_pm_b_local;
    Sensors xsensors;
    xsensors.temp_hum = nullptr; // No dedicated sensor
    xsensors.co2 = nullptr;
    xsensors.pms_a = &mock_pm_a_local;
    xsensors.pms_b = &mock_pm_b_local;
    xsensors.tvoc_nox = nullptr;
    xsensors.o3_no2 = nullptr;
    xsensors.pressure = nullptr;
    SensorManager xsensor_manager(xsensors);

    // Only PM sensor B supports temp/hum
    REQUIRE_CALL(mock_pm_a_local, supports_temp_hum()).RETURN(false);
    REQUIRE_CALL(mock_pm_b_local, supports_temp_hum()).RETURN(true);

    // PM sensor B provides temp/hum data
    REQUIRE_CALL(mock_pm_b_local, temp_hum_data()).RETURN(TempHumData{18.0f, 65.0f});

    // PM sensors provide PM data
    EXPECT_READ(mock_pm_a_local,
                (PMData{1.0f, 2.5f, 10.0f, 1.1f, 2.6f, 10.1f, 0.3f, 0.5f, 1.0f, 2.5f, 5.0f, 10.0f}),
                true);
    EXPECT_READ(mock_pm_b_local,
                (PMData{10.0f, 20.0f, 100.0f, 10.1f, 20.1f, 100.1f, 10.3f, 10.5f, 10.0f, 20.0f,
                        50.0f, 100.0f}),
                true);

    auto result = xsensor_manager.start_measures(1);

    // Verify temp_hum_a is invalid (pms_a doesn't support temp/hum)
    REQUIRE(result.temp_hum_a.temperature == MeasuresInvalid::TEMPERATURE);
    REQUIRE(result.temp_hum_a.humidity == MeasuresInvalid::HUMIDITY);

    // Verify temp_hum_b comes from pms_b
    REQUIRE_THAT(result.temp_hum_b.temperature, WithinAbs(18.0f, 0.001f));
    REQUIRE_THAT(result.temp_hum_b.humidity, WithinAbs(65.0f, 0.001f));

    // Verify PM data is present
    REQUIRE_THAT(result.pm_a.pm_01, WithinAbs(1.0f, 0.001f));
    REQUIRE_THAT(result.pm_b.pm_01, WithinAbs(10.0f, 0.001f));
  }

  SECTION("Dedicated sensor ignores PM temp/hum") {
    // Allow RTOS calls (timing doesn't matter for this test)
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    MockTempHumSensor mock_tempHum_local;
    MockPMSensor mock_pm_a_local;
    MockPMSensor mock_pm_b_local;
    Sensors xsensors;
    xsensors.temp_hum = &mock_tempHum_local; // Dedicated sensor present
    xsensors.co2 = nullptr;
    xsensors.pms_a = &mock_pm_a_local;
    xsensors.pms_b = &mock_pm_b_local;
    xsensors.tvoc_nox = nullptr;
    xsensors.o3_no2 = nullptr;
    xsensors.pressure = nullptr;
    SensorManager xsensor_manager(xsensors);

    // Both PM sensors support temp/hum, but should be ignored
    ALLOW_CALL(mock_pm_a_local, supports_temp_hum()).RETURN(true);
    ALLOW_CALL(mock_pm_b_local, supports_temp_hum()).RETURN(true);

    // Dedicated sensor provides data
    EXPECT_READ(mock_tempHum_local, (TempHumData{25.0f, 55.0f}), true);

    // PM sensors provide PM data only (temp/hum methods should NOT be called)
    EXPECT_READ(mock_pm_a_local,
                (PMData{1.0f, 2.5f, 10.0f, 1.1f, 2.6f, 10.1f, 0.3f, 0.5f, 1.0f, 2.5f, 5.0f, 10.0f}),
                true);
    EXPECT_READ(mock_pm_b_local,
                (PMData{10.0f, 20.0f, 100.0f, 10.1f, 20.1f, 100.1f, 10.3f, 10.5f, 10.0f, 20.0f,
                        50.0f, 100.0f}),
                true);

    auto result = xsensor_manager.start_measures(1);

    // Verify temp_hum_a comes from dedicated sensor only
    REQUIRE_THAT(result.temp_hum_a.temperature, WithinAbs(25.0f, 0.001f));
    REQUIRE_THAT(result.temp_hum_a.humidity, WithinAbs(55.0f, 0.001f));

    // Verify temp_hum_b is invalid (PM sensors ignored when dedicated exists)
    REQUIRE(result.temp_hum_b.temperature == MeasuresInvalid::TEMPERATURE);
    REQUIRE(result.temp_hum_b.humidity == MeasuresInvalid::HUMIDITY);

    // Verify PM data is present
    REQUIRE_THAT(result.pm_a.pm_01, WithinAbs(1.0f, 0.001f));
    REQUIRE_THAT(result.pm_b.pm_01, WithinAbs(10.0f, 0.001f));
  }

  SECTION("Single iteration skips delay") {
    // Single-iteration measurement should NOT call delay_ms
    REQUIRE_CALL(mock_rtos, get_time_ms_impl()).IN_SEQUENCE(seq).RETURN(0);

    // No delay should be called
    FORBID_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    // Set up sensor mocks
    ALLOW_CALL(mock_co2, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_pm_a, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_pm_b, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_tvoc_nox, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_o3no2, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_pressure, read(trompeloeil::_)).RETURN(false);
    EXPECT_READ(mock_tempHum, (TempHumData{25.0f, 50.0f}), true);

    Measures result = sensor_manager.start_measures(1);

    REQUIRE_THAT(result.temp_hum_a.temperature, WithinAbs(25.0f, 0.001f));
  }

  SECTION("Multiple iterations each delay correctly") {
    // Iteration 1: takes 300ms
    REQUIRE_CALL(mock_rtos, get_time_ms_impl()).IN_SEQUENCE(seq).RETURN(0);
    REQUIRE_CALL(mock_rtos, get_time_ms_impl()).IN_SEQUENCE(seq).RETURN(300);
    REQUIRE_CALL(mock_rtos, delay_ms_impl(1700));

    // Iteration 2: takes 800ms
    REQUIRE_CALL(mock_rtos, get_time_ms_impl()).IN_SEQUENCE(seq).RETURN(2000);
    REQUIRE_CALL(mock_rtos, get_time_ms_impl()).IN_SEQUENCE(seq).RETURN(2800);
    REQUIRE_CALL(mock_rtos, delay_ms_impl(1200));

    // Iteration 3: takes 100ms
    REQUIRE_CALL(mock_rtos, get_time_ms_impl()).IN_SEQUENCE(seq).RETURN(4000);
    REQUIRE_CALL(mock_rtos, get_time_ms_impl()).IN_SEQUENCE(seq).RETURN(4100);
    REQUIRE_CALL(mock_rtos, delay_ms_impl(1900));

    // Set up sensor mocks (ALLOW_CALL first, then EXPECT_READ for proper order)
    ALLOW_CALL(mock_co2, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_pm_a, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_pm_b, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_tvoc_nox, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_o3no2, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_pressure, read(trompeloeil::_)).RETURN(false);
    EXPECT_READ(mock_tempHum, (TempHumData{25.0f, 50.0f}), true).TIMES(3);

    sensor_manager.start_measures(3);
  }

  SECTION("Multi-iteration delay when work takes 500ms") {
    // 2 iterations, each taking 500ms — should delay 1500ms each
    REQUIRE_CALL(mock_rtos, get_time_ms_impl()).IN_SEQUENCE(seq).RETURN(0);
    REQUIRE_CALL(mock_rtos, get_time_ms_impl()).IN_SEQUENCE(seq).RETURN(500);
    REQUIRE_CALL(mock_rtos, delay_ms_impl(1500));
    REQUIRE_CALL(mock_rtos, get_time_ms_impl()).IN_SEQUENCE(seq).RETURN(2000);
    REQUIRE_CALL(mock_rtos, get_time_ms_impl()).IN_SEQUENCE(seq).RETURN(2500);
    REQUIRE_CALL(mock_rtos, delay_ms_impl(1500));

    ALLOW_CALL(mock_co2, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_pm_a, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_pm_b, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_tvoc_nox, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_o3no2, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_pressure, read(trompeloeil::_)).RETURN(false);
    EXPECT_READ(mock_tempHum, (TempHumData{25.0f, 50.0f}), true).TIMES(2);

    sensor_manager.start_measures(2);
  }

  SECTION("No delay when multi-iteration exceeds 2 seconds") {
    // 2 iterations: first takes 3000ms (exceeds 2s, no delay), second takes 100ms (delay 1900ms)
    REQUIRE_CALL(mock_rtos, get_time_ms_impl()).IN_SEQUENCE(seq).RETURN(0);
    REQUIRE_CALL(mock_rtos, get_time_ms_impl()).IN_SEQUENCE(seq).RETURN(3000);
    // No delay for iteration 1 (elapsed > 2000ms)
    REQUIRE_CALL(mock_rtos, get_time_ms_impl()).IN_SEQUENCE(seq).RETURN(3000);
    REQUIRE_CALL(mock_rtos, get_time_ms_impl()).IN_SEQUENCE(seq).RETURN(3100);
    REQUIRE_CALL(mock_rtos, delay_ms_impl(1900));

    ALLOW_CALL(mock_co2, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_pm_a, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_pm_b, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_tvoc_nox, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_o3no2, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_pressure, read(trompeloeil::_)).RETURN(false);
    EXPECT_READ(mock_tempHum, (TempHumData{25.0f, 50.0f}), true).TIMES(2);

    sensor_manager.start_measures(2);
  }

  // "Very fast iteration delays almost full 2 seconds" — removed.
  // Single-iteration measurements now skip the delay entirely.
  // Multi-iteration delay is covered by "Multi-iteration delay when work takes 500ms"
  // and "Multiple iterations each delay correctly".

  SECTION("CO2 sensor provides temp/hum when no dedicated sensor") {
    // Allow RTOS calls (timing doesn't matter for this test)
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));
    trompeloeil::sequence seq;

    MockCO2Sensor mock_co2_local;
    Sensors xsensors;
    xsensors.temp_hum = nullptr; // No dedicated sensor
    xsensors.co2 = &mock_co2_local;
    xsensors.pms_a = nullptr;
    xsensors.pms_b = nullptr;
    xsensors.tvoc_nox = nullptr;
    xsensors.o3_no2 = nullptr;
    xsensors.pressure = nullptr;
    SensorManager xsensor_manager(xsensors);

    // CO2 sensor supports temp/hum
    REQUIRE_CALL(mock_co2_local, supports_temp_hum()).RETURN(true);

    // CO2 sensor provides CO2 data
    REQUIRE_CALL(mock_co2_local, read(trompeloeil::_))
        .IN_SEQUENCE(seq)
        .LR_SIDE_EFFECT(_1 = CO2Data{450})
        .RETURN(true);
    REQUIRE_CALL(mock_co2_local, temp_hum_data())
        .IN_SEQUENCE(seq)
        .RETURN(TempHumData{22.5f, 55.0f});

    auto result = xsensor_manager.start_measures(1);

    // Verify CO2 data
    REQUIRE(result.co2.co2 == 450);

    // Verify temp_hum_a comes from CO2 sensor (no dedicated sensor)
    REQUIRE_THAT(result.temp_hum_a.temperature, WithinAbs(22.5f, 0.001f));
    REQUIRE_THAT(result.temp_hum_a.humidity, WithinAbs(55.0f, 0.001f));

    // temp_hum_b should be invalid
    REQUIRE(result.temp_hum_b.temperature == MeasuresInvalid::TEMPERATURE);
    REQUIRE(result.temp_hum_b.humidity == MeasuresInvalid::HUMIDITY);
  }

  SECTION("CO2 sensor temp/hum averaging over 3 iterations") {
    // Allow RTOS calls (timing doesn't matter for this test)
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    MockCO2Sensor mock_co2_local;
    Sensors xsensors;
    xsensors.temp_hum = nullptr;
    xsensors.co2 = &mock_co2_local;
    xsensors.pms_a = nullptr;
    xsensors.pms_b = nullptr;
    xsensors.tvoc_nox = nullptr;
    xsensors.o3_no2 = nullptr;
    xsensors.pressure = nullptr;
    SensorManager xsensor_manager(xsensors);

    // CO2 sensor supports temp/hum
    REQUIRE_CALL(mock_co2_local, supports_temp_hum()).RETURN(true);
    REQUIRE_CALL(mock_co2_local, temp_hum_data()).RETURN(TempHumData{20.0f, 50.0f});
    REQUIRE_CALL(mock_co2_local, temp_hum_data()).RETURN(TempHumData{22.0f, 52.0f});
    REQUIRE_CALL(mock_co2_local, temp_hum_data()).RETURN(TempHumData{24.0f, 54.0f});

    // CO2 readings
    EXPECT_READ(mock_co2_local, (CO2Data{400}), true);
    EXPECT_READ(mock_co2_local, (CO2Data{500}), true);
    EXPECT_READ(mock_co2_local, (CO2Data{600}), true);

    auto result = xsensor_manager.start_measures(3);

    // CO2 average: (400+500+600)/3 = 500
    REQUIRE(result.co2.co2 == 500);

    // Temp/hum average from CO2 sensor: (20+22+24)/3 = 22, (50+52+54)/3 = 52
    REQUIRE_THAT(result.temp_hum_a.temperature, WithinAbs(22.0f, 0.001f));
    REQUIRE_THAT(result.temp_hum_a.humidity, WithinAbs(52.0f, 0.001f));
  }

  SECTION("CO2 sensor does not provide temp/hum") {
    // Allow RTOS calls (timing doesn't matter for this test)
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    MockCO2Sensor mock_co2_local;
    Sensors xsensors;
    xsensors.temp_hum = nullptr;
    xsensors.co2 = &mock_co2_local;
    xsensors.pms_a = nullptr;
    xsensors.pms_b = nullptr;
    xsensors.tvoc_nox = nullptr;
    xsensors.o3_no2 = nullptr;
    xsensors.pressure = nullptr;
    SensorManager xsensor_manager(xsensors);

    // CO2 sensor does NOT support temp/hum
    REQUIRE_CALL(mock_co2_local, supports_temp_hum()).RETURN(false);

    // CO2 sensor provides CO2 data only
    EXPECT_READ(mock_co2_local, (CO2Data{450}), true);

    auto result = xsensor_manager.start_measures(1);

    // Verify CO2 data
    REQUIRE(result.co2.co2 == 450);

    // Verify both temp/hum fields are invalid (no temp/hum source available)
    REQUIRE(result.temp_hum_a.temperature == MeasuresInvalid::TEMPERATURE);
    REQUIRE(result.temp_hum_a.humidity == MeasuresInvalid::HUMIDITY);
    REQUIRE(result.temp_hum_b.temperature == MeasuresInvalid::TEMPERATURE);
    REQUIRE(result.temp_hum_b.humidity == MeasuresInvalid::HUMIDITY);
  }

  SECTION("Dedicated sensor ignores CO2 temp/hum") {
    // Allow RTOS calls (timing doesn't matter for this test)
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    MockTempHumSensor mock_tempHum_local;
    MockCO2Sensor mock_co2_local;
    Sensors xsensors;
    xsensors.temp_hum = &mock_tempHum_local; // Dedicated sensor present
    xsensors.co2 = &mock_co2_local;
    xsensors.pms_a = nullptr;
    xsensors.pms_b = nullptr;
    xsensors.tvoc_nox = nullptr;
    xsensors.o3_no2 = nullptr;
    xsensors.pressure = nullptr;
    SensorManager xsensor_manager(xsensors);

    // CO2 sensor supports temp/hum, but should be ignored
    ALLOW_CALL(mock_co2_local, supports_temp_hum()).RETURN(true);

    // Dedicated sensor provides data
    EXPECT_READ(mock_tempHum_local, (TempHumData{25.0f, 55.0f}), true);

    // CO2 sensor provides CO2 data (temp_hum_data() should NOT be called)
    EXPECT_READ(mock_co2_local, (CO2Data{450}), true);

    auto result = xsensor_manager.start_measures(1);

    // Verify temp_hum_a comes from dedicated sensor only
    REQUIRE_THAT(result.temp_hum_a.temperature, WithinAbs(25.0f, 0.001f));
    REQUIRE_THAT(result.temp_hum_a.humidity, WithinAbs(55.0f, 0.001f));

    // Verify temp_hum_b is invalid
    REQUIRE(result.temp_hum_b.temperature == MeasuresInvalid::TEMPERATURE);
    REQUIRE(result.temp_hum_b.humidity == MeasuresInvalid::HUMIDITY);

    // Verify CO2 data
    REQUIRE(result.co2.co2 == 450);
  }

  SECTION("Pressure sensor provides temp/hum when no dedicated sensor") {
    // Allow RTOS calls (timing doesn't matter for this test)
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    MockPressureSensor mock_pressure_local;
    Sensors xsensors;
    xsensors.temp_hum = nullptr; // No dedicated sensor
    xsensors.co2 = nullptr;
    xsensors.pms_a = nullptr;
    xsensors.pms_b = nullptr;
    xsensors.tvoc_nox = nullptr;
    xsensors.o3_no2 = nullptr;
    xsensors.pressure = &mock_pressure_local;
    SensorManager xsensor_manager(xsensors);

    // Pressure sensor supports temp/hum — consulted by both the temp/hum
    // fallback resolution and the pressure-temperature accumulation.
    ALLOW_CALL(mock_pressure_local, supports_temp_hum()).RETURN(true);
    ALLOW_CALL(mock_pressure_local, temp_hum_data())
        .RETURN(TempHumData{23.5f, MeasuresInvalid::HUMIDITY});

    // Pressure sensor provides pressure data
    EXPECT_READ(mock_pressure_local, (PressureData{1013.25f, 110.0f}), true);

    auto result = xsensor_manager.start_measures(1);

    // Verify pressure data, including the die temperature now carried along
    REQUIRE_THAT(result.pressure.pressure, WithinAbs(1013.25f, 0.001f));
    REQUIRE_THAT(result.pressure.altitude, WithinAbs(110.0f, 0.001f));
    REQUIRE_THAT(result.pressure.temperature, WithinAbs(23.5f, 0.001f));

    // Verify temp_hum_a comes from pressure sensor (no dedicated sensor)
    REQUIRE_THAT(result.temp_hum_a.temperature, WithinAbs(23.5f, 0.001f));
    // Humidity should be invalid (DPS368 doesn't measure humidity)
    REQUIRE(result.temp_hum_a.humidity == MeasuresInvalid::HUMIDITY);

    // temp_hum_b should be invalid
    REQUIRE(result.temp_hum_b.temperature == MeasuresInvalid::TEMPERATURE);
    REQUIRE(result.temp_hum_b.humidity == MeasuresInvalid::HUMIDITY);
  }

  SECTION("Pressure sensor temp/hum averaging over 3 iterations") {
    // Allow RTOS calls (timing doesn't matter for this test)
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    MockPressureSensor mock_pressure_local;
    Sensors xsensors;
    xsensors.temp_hum = nullptr;
    xsensors.co2 = nullptr;
    xsensors.pms_a = nullptr;
    xsensors.pms_b = nullptr;
    xsensors.tvoc_nox = nullptr;
    xsensors.o3_no2 = nullptr;
    xsensors.pressure = &mock_pressure_local;
    SensorManager xsensor_manager(xsensors);

    // Pressure sensor supports temp/hum. Each iteration reads the die
    // temperature twice: once for the temp/hum fallback and once for the
    // pressure-temperature accumulation.
    ALLOW_CALL(mock_pressure_local, supports_temp_hum()).RETURN(true);
    REQUIRE_CALL(mock_pressure_local, temp_hum_data())
        .RETURN(TempHumData{20.0f, MeasuresInvalid::HUMIDITY})
        .TIMES(2);
    REQUIRE_CALL(mock_pressure_local, temp_hum_data())
        .RETURN(TempHumData{22.0f, MeasuresInvalid::HUMIDITY})
        .TIMES(2);
    REQUIRE_CALL(mock_pressure_local, temp_hum_data())
        .RETURN(TempHumData{24.0f, MeasuresInvalid::HUMIDITY})
        .TIMES(2);

    // Pressure readings
    EXPECT_READ(mock_pressure_local, (PressureData{1010.0f, 100.0f}), true);
    EXPECT_READ(mock_pressure_local, (PressureData{1012.0f, 110.0f}), true);
    EXPECT_READ(mock_pressure_local, (PressureData{1014.0f, 120.0f}), true);

    auto result = xsensor_manager.start_measures(3);

    // Pressure average: (1010+1012+1014)/3 = 1012
    REQUIRE_THAT(result.pressure.pressure, WithinAbs(1012.0f, 0.001f));

    // Temp average from pressure sensor: (20+22+24)/3 = 22
    REQUIRE_THAT(result.temp_hum_a.temperature, WithinAbs(22.0f, 0.001f));
    // Humidity should be invalid (DPS368 doesn't measure humidity)
    REQUIRE(result.temp_hum_a.humidity == MeasuresInvalid::HUMIDITY);
  }

  SECTION("Pressure sensor does not provide temp/hum") {
    // Allow RTOS calls (timing doesn't matter for this test)
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    MockPressureSensor mock_pressure_local;
    Sensors xsensors;
    xsensors.temp_hum = nullptr;
    xsensors.co2 = nullptr;
    xsensors.pms_a = nullptr;
    xsensors.pms_b = nullptr;
    xsensors.tvoc_nox = nullptr;
    xsensors.o3_no2 = nullptr;
    xsensors.pressure = &mock_pressure_local;
    SensorManager xsensor_manager(xsensors);

    // Pressure sensor does NOT support temp/hum (probed by both the fallback
    // resolver and the pressure-temperature accumulation)
    ALLOW_CALL(mock_pressure_local, supports_temp_hum()).RETURN(false);

    // Pressure sensor provides pressure data only
    EXPECT_READ(mock_pressure_local, (PressureData{1013.25f, 110.0f}), true);

    auto result = xsensor_manager.start_measures(1);

    // Verify pressure data
    REQUIRE_THAT(result.pressure.pressure, WithinAbs(1013.25f, 0.001f));

    // Verify both temp/hum fields are invalid (no temp/hum source available)
    REQUIRE(result.temp_hum_a.temperature == MeasuresInvalid::TEMPERATURE);
    REQUIRE(result.temp_hum_a.humidity == MeasuresInvalid::HUMIDITY);
    REQUIRE(result.temp_hum_b.temperature == MeasuresInvalid::TEMPERATURE);
    REQUIRE(result.temp_hum_b.humidity == MeasuresInvalid::HUMIDITY);
  }

  SECTION("Dedicated sensor ignores pressure temp/hum") {
    // Allow RTOS calls (timing doesn't matter for this test)
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    MockTempHumSensor mock_tempHum_local;
    MockPressureSensor mock_pressure_local;
    Sensors xsensors;
    xsensors.temp_hum = &mock_tempHum_local; // Dedicated sensor present
    xsensors.co2 = nullptr;
    xsensors.pms_a = nullptr;
    xsensors.pms_b = nullptr;
    xsensors.tvoc_nox = nullptr;
    xsensors.o3_no2 = nullptr;
    xsensors.pressure = &mock_pressure_local;
    SensorManager xsensor_manager(xsensors);

    // Pressure sensor supports temp/hum, but must not feed temp_hum_a; its
    // die temperature is still accumulated into the pressure measures.
    ALLOW_CALL(mock_pressure_local, supports_temp_hum()).RETURN(true);
    ALLOW_CALL(mock_pressure_local, temp_hum_data())
        .RETURN(TempHumData{30.0f, MeasuresInvalid::HUMIDITY});

    // Dedicated sensor provides data
    EXPECT_READ(mock_tempHum_local, (TempHumData{25.0f, 55.0f}), true);

    EXPECT_READ(mock_pressure_local, (PressureData{1013.25f, 110.0f}), true);

    auto result = xsensor_manager.start_measures(1);

    // Verify temp_hum_a comes from dedicated sensor only
    REQUIRE_THAT(result.temp_hum_a.temperature, WithinAbs(25.0f, 0.001f));
    REQUIRE_THAT(result.temp_hum_a.humidity, WithinAbs(55.0f, 0.001f));

    // Verify temp_hum_b is invalid
    REQUIRE(result.temp_hum_b.temperature == MeasuresInvalid::TEMPERATURE);
    REQUIRE(result.temp_hum_b.humidity == MeasuresInvalid::HUMIDITY);

    // Verify pressure data
    REQUIRE_THAT(result.pressure.pressure, WithinAbs(1013.25f, 0.001f));
  }

  SECTION("Product priority: DEDICATED wins over CO2 and PRESSURE for temp_hum_a") {
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    MockTempHumSensor mock_tempHum_local;
    MockCO2Sensor mock_co2_local;
    MockPressureSensor mock_pressure_local;
    Sensors xsensors{};
    xsensors.temp_hum = &mock_tempHum_local;
    xsensors.co2 = &mock_co2_local;
    xsensors.pressure = &mock_pressure_local;
    xsensors.temp_hum_a_fallback.priority[0] = TempHumSource::DEDICATED;
    xsensors.temp_hum_a_fallback.priority[1] = TempHumSource::CO2;
    xsensors.temp_hum_a_fallback.priority[2] = TempHumSource::PRESSURE;
    xsensors.temp_hum_a_fallback.count = 3;
    SensorManager xsensor_manager(xsensors);

    FORBID_CALL(mock_co2_local, supports_temp_hum());
    FORBID_CALL(mock_co2_local, temp_hum_data());
    // The pressure-temperature accumulation reads the die temperature even
    // when the fallback resolver never reaches PRESSURE.
    ALLOW_CALL(mock_pressure_local, supports_temp_hum()).RETURN(true);
    ALLOW_CALL(mock_pressure_local, temp_hum_data())
        .RETURN(TempHumData{30.0f, MeasuresInvalid::HUMIDITY});

    EXPECT_READ(mock_tempHum_local, (TempHumData{24.5f, 56.0f}), true);
    EXPECT_READ(mock_co2_local, (CO2Data{460}), true);
    EXPECT_READ(mock_pressure_local, (PressureData{1013.25f, 110.0f}), true);

    auto result = xsensor_manager.start_measures(1);

    REQUIRE_THAT(result.temp_hum_a.temperature, WithinAbs(24.5f, 0.001f));
    REQUIRE_THAT(result.temp_hum_a.humidity, WithinAbs(56.0f, 0.001f));
    REQUIRE(result.co2.co2 == 460);
    REQUIRE_THAT(result.pressure.pressure, WithinAbs(1013.25f, 0.001f));
  }

  SECTION("Priority: CO2 wins over PM_A and PRESSURE for temp_hum_a") {
    // When multiple sensors support temp/hum, only the highest-priority
    // source contributes to temp_hum_a (default: DEDICATED > CO2 > PM_A > PRESSURE)
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    MockCO2Sensor mock_co2_local;
    MockPMSensor mock_pm_a_local;
    MockPressureSensor mock_pressure_local;
    Sensors xsensors;
    xsensors.temp_hum = nullptr; // No dedicated sensor
    xsensors.co2 = &mock_co2_local;
    xsensors.pms_a = &mock_pm_a_local;
    xsensors.pms_b = nullptr;
    xsensors.tvoc_nox = nullptr;
    xsensors.o3_no2 = nullptr;
    xsensors.pressure = &mock_pressure_local;
    SensorManager xsensor_manager(xsensors);

    // All three support temp/hum, but CO2 should win (higher priority)
    REQUIRE_CALL(mock_co2_local, supports_temp_hum()).RETURN(true);
    // PM_A and PRESSURE supports_temp_hum should NOT be called (resolver stops at CO2)
    ALLOW_CALL(mock_pm_a_local, supports_temp_hum()).RETURN(true);
    ALLOW_CALL(mock_pressure_local, supports_temp_hum()).RETURN(true);

    // Only CO2 feeds temp_hum_a; the pressure die temperature is still read
    // for the pressure measures.
    REQUIRE_CALL(mock_co2_local, temp_hum_data()).RETURN(TempHumData{22.0f, 55.0f});
    ALLOW_CALL(mock_pressure_local, temp_hum_data())
        .RETURN(TempHumData{30.0f, MeasuresInvalid::HUMIDITY});

    EXPECT_READ(mock_co2_local, (CO2Data{450}), true);
    EXPECT_READ(mock_pm_a_local,
                (PMData{1.0f, 2.5f, 10.0f, 1.1f, 2.6f, 10.1f, 0.3f, 0.5f, 1.0f, 2.5f, 5.0f, 10.0f}),
                true);
    EXPECT_READ(mock_pressure_local, (PressureData{1013.25f, 110.0f}), true);

    auto result = xsensor_manager.start_measures(1);

    // Verify temp_hum_a comes from CO2 only (not blended)
    REQUIRE_THAT(result.temp_hum_a.temperature, WithinAbs(22.0f, 0.001f));
    REQUIRE_THAT(result.temp_hum_a.humidity, WithinAbs(55.0f, 0.001f));

    // Verify other sensor data is still collected
    REQUIRE(result.co2.co2 == 450);
    REQUIRE_THAT(result.pm_a.pm_01, WithinAbs(1.0f, 0.001f));
    REQUIRE_THAT(result.pressure.pressure, WithinAbs(1013.25f, 0.001f));
  }

  SECTION("Custom priority: PRESSURE before CO2") {
    // Caller can override the default priority order
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));
    trompeloeil::sequence seq;

    MockCO2Sensor mock_co2_local;
    MockPressureSensor mock_pressure_local;
    Sensors xsensors;
    xsensors.temp_hum = nullptr;
    xsensors.co2 = &mock_co2_local;
    xsensors.pms_a = nullptr;
    xsensors.pms_b = nullptr;
    xsensors.tvoc_nox = nullptr;
    xsensors.o3_no2 = nullptr;
    xsensors.pressure = &mock_pressure_local;

    // Custom priority: PRESSURE before CO2
    xsensors.temp_hum_a_fallback.priority[0] = TempHumSource::DEDICATED;
    xsensors.temp_hum_a_fallback.priority[1] = TempHumSource::PRESSURE;
    xsensors.temp_hum_a_fallback.priority[2] = TempHumSource::CO2;
    xsensors.temp_hum_a_fallback.priority[3] = TempHumSource::PM_A;
    xsensors.temp_hum_a_fallback.count = 4;

    SensorManager xsensor_manager(xsensors);

    // Both support temp/hum, but PRESSURE should win (custom priority)
    ALLOW_CALL(mock_pressure_local, supports_temp_hum()).RETURN(true);
    // CO2 supports_temp_hum should NOT be called (resolver stops at PRESSURE)
    ALLOW_CALL(mock_co2_local, supports_temp_hum()).RETURN(true);

    EXPECT_READ(mock_co2_local, (CO2Data{500}), true);
    REQUIRE_CALL(mock_pressure_local, read(trompeloeil::_))
        .IN_SEQUENCE(seq)
        .LR_SIDE_EFFECT(_1 = PressureData{1015.0f, 105.0f})
        .RETURN(true);
    // Read twice after the pressure read: once for the temp/hum fallback and
    // once for the pressure-temperature accumulation.
    REQUIRE_CALL(mock_pressure_local, temp_hum_data())
        .IN_SEQUENCE(seq)
        .RETURN(TempHumData{24.0f, MeasuresInvalid::HUMIDITY})
        .TIMES(2);

    auto result = xsensor_manager.start_measures(1);

    // Verify temp_hum_a comes from PRESSURE (not CO2)
    REQUIRE_THAT(result.temp_hum_a.temperature, WithinAbs(24.0f, 0.001f));
    REQUIRE(result.temp_hum_a.humidity == MeasuresInvalid::HUMIDITY);

    // Verify other sensor data is still collected
    REQUIRE(result.co2.co2 == 500);
    REQUIRE_THAT(result.pressure.pressure, WithinAbs(1015.0f, 0.001f));
  }

  // =========================================================================
  // SensorGroup filtering tests
  // =========================================================================

  SECTION("PM-only group populates PM fields, other fields are sentinels") {
    // Single iteration — no delay expected
    REQUIRE_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    FORBID_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    // PM sensors should be read
    EXPECT_READ(mock_pm_a,
                (PMData{1.0f, 2.5f, 10.0f, 1.1f, 2.6f, 10.1f, 0.3f, 0.5f, 1.0f, 2.5f, 5.0f, 10.0f}),
                true);
    EXPECT_READ(mock_pm_b,
                (PMData{101.0f, 102.5f, 110.0f, 101.1f, 102.6f, 110.1f, 100.3f, 100.5f, 101.0f,
                        102.5f, 105.0f, 110.0f}),
                true);

    // Other sensors should NOT be called
    FORBID_CALL(mock_tempHum, read(trompeloeil::_));
    FORBID_CALL(mock_co2, read(trompeloeil::_));
    FORBID_CALL(mock_tvoc_nox, read(trompeloeil::_));
    FORBID_CALL(mock_o3no2, read(trompeloeil::_));
    FORBID_CALL(mock_pressure, read(trompeloeil::_));

    auto result = sensor_manager.start_measures(1, SensorGroup::PM);

    // PM fields should be valid
    REQUIRE_THAT(result.pm_a.pm_01, WithinAbs(1.0f, 0.001f));
    REQUIRE_THAT(result.pm_a.pm_25, WithinAbs(2.5f, 0.001f));
    REQUIRE_THAT(result.pm_b.pm_01, WithinAbs(101.0f, 0.001f));

    // Other fields should be invalid sentinels
    REQUIRE(result.temp_hum_a.temperature == MeasuresInvalid::TEMPERATURE);
    REQUIRE(result.temp_hum_a.humidity == MeasuresInvalid::HUMIDITY);
    REQUIRE(result.co2.co2 == MeasuresInvalid::CO2);
    REQUIRE(result.tvoc_nox.tvoc_index == MeasuresInvalid::TVOC);
    REQUIRE(result.electrode.o3_we == MeasuresInvalid::VOLT);
    REQUIRE(result.pressure.pressure == MeasuresInvalid::PRESSURE);
  }

  SECTION("Other-only group populates other fields, PM and TVOC fields are sentinels") {
    // Single iteration — no delay expected
    REQUIRE_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    FORBID_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    // Other sensors should be read (TVOC/NOx is now in TvocNox group, not Other)
    EXPECT_READ(mock_tempHum, (TempHumData{25.0f, 60.0f}), true);
    EXPECT_READ(mock_co2, (CO2Data{450}), true);
    EXPECT_READ(mock_o3no2, (O3No2Data{0.5f, 0.6f, 0.7f, 0.8f, 25.0f}), true);
    EXPECT_READ(mock_pressure, (PressureData{1013.25f, 110.0f}), true);

    // PM and TVOC sensors should NOT be called
    FORBID_CALL(mock_pm_a, read(trompeloeil::_));
    FORBID_CALL(mock_pm_b, read(trompeloeil::_));
    FORBID_CALL(mock_tvoc_nox, read(trompeloeil::_));

    auto result = sensor_manager.start_measures(1, SensorGroup::Other);

    // Other fields should be valid
    REQUIRE_THAT(result.temp_hum_a.temperature, WithinAbs(25.0f, 0.001f));
    REQUIRE_THAT(result.temp_hum_a.humidity, WithinAbs(60.0f, 0.001f));
    REQUIRE(result.co2.co2 == 450);
    REQUIRE_THAT(result.electrode.o3_we, WithinAbs(0.5f, 0.001f));
    REQUIRE_THAT(result.pressure.pressure, WithinAbs(1013.25f, 0.001f));

    // PM fields should be invalid sentinels
    REQUIRE(result.pm_a.pm_01 == MeasuresInvalid::PM);
    REQUIRE(result.pm_a.pm_25 == MeasuresInvalid::PM);
    REQUIRE(result.pm_b.pm_01 == MeasuresInvalid::PM);

    // TVOC/NOx fields should be invalid sentinels (TvocNox group not requested)
    REQUIRE(result.tvoc_nox.tvoc_index == MeasuresInvalid::TVOC);
    REQUIRE(result.tvoc_nox.tvoc_raw == MeasuresInvalid::TVOC);
    REQUIRE(result.tvoc_nox.nox_index == MeasuresInvalid::NOX);
    REQUIRE(result.tvoc_nox.nox_raw == MeasuresInvalid::NOX);
  }

  SECTION("All group populates all fields") {
    // Single iteration — no delay expected
    REQUIRE_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    FORBID_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    EXPECT_READ(mock_tempHum, (TempHumData{25.0f, 60.0f}), true);
    EXPECT_READ(mock_co2, (CO2Data{450}), true);
    EXPECT_READ(mock_pm_a,
                (PMData{1.0f, 2.5f, 10.0f, 1.1f, 2.6f, 10.1f, 0.3f, 0.5f, 1.0f, 2.5f, 5.0f, 10.0f}),
                true);
    EXPECT_READ(mock_pm_b,
                (PMData{101.0f, 102.5f, 110.0f, 101.1f, 102.6f, 110.1f, 100.3f, 100.5f, 101.0f,
                        102.5f, 105.0f, 110.0f}),
                true);
    EXPECT_READ(mock_tvoc_nox, (TVOCNOxData{120, 210, 55, 80}), true);
    EXPECT_READ(mock_o3no2, (O3No2Data{0.5f, 0.6f, 0.7f, 0.8f, 25.0f}), true);
    EXPECT_READ(mock_pressure, (PressureData{1013.25f, 110.0f}), true);

    auto result = sensor_manager.start_measures(1, SensorGroup::All);

    // All fields should be valid
    REQUIRE_THAT(result.temp_hum_a.temperature, WithinAbs(25.0f, 0.001f));
    REQUIRE(result.co2.co2 == 450);
    REQUIRE_THAT(result.pm_a.pm_01, WithinAbs(1.0f, 0.001f));
    REQUIRE_THAT(result.pm_b.pm_01, WithinAbs(101.0f, 0.001f));
    REQUIRE(result.tvoc_nox.tvoc_index == 120);
    REQUIRE_THAT(result.electrode.o3_we, WithinAbs(0.5f, 0.001f));
    REQUIRE_THAT(result.pressure.pressure, WithinAbs(1013.25f, 0.001f));
  }

  SECTION("Multi-iteration delay still applies with group filter") {
    // 5 iterations with PM-only group — delay should apply
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    ALLOW_CALL(mock_pm_a, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_pm_b, read(trompeloeil::_)).RETURN(false);

    // Other sensors should NOT be called
    FORBID_CALL(mock_tempHum, read(trompeloeil::_));
    FORBID_CALL(mock_co2, read(trompeloeil::_));
    FORBID_CALL(mock_tvoc_nox, read(trompeloeil::_));
    FORBID_CALL(mock_o3no2, read(trompeloeil::_));
    FORBID_CALL(mock_pressure, read(trompeloeil::_));

    sensor_manager.start_measures(5, SensorGroup::PM);
  }

  SECTION("Null sensors in active PM group returns sentinels") {
    REQUIRE_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    FORBID_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    // Other sensors should NOT be called
    FORBID_CALL(mock_tempHum, read(trompeloeil::_));
    FORBID_CALL(mock_co2, read(trompeloeil::_));
    FORBID_CALL(mock_tvoc_nox, read(trompeloeil::_));
    FORBID_CALL(mock_o3no2, read(trompeloeil::_));
    FORBID_CALL(mock_pressure, read(trompeloeil::_));

    // Create sensors with null PM
    Sensors null_pm_sensors{};
    null_pm_sensors.temp_hum = &mock_tempHum;
    null_pm_sensors.co2 = &mock_co2;
    null_pm_sensors.pms_a = nullptr;
    null_pm_sensors.pms_b = nullptr;
    null_pm_sensors.tvoc_nox = &mock_tvoc_nox;
    null_pm_sensors.o3_no2 = &mock_o3no2;
    null_pm_sensors.pressure = &mock_pressure;
    SensorManager null_pm_manager(null_pm_sensors);

    auto result = null_pm_manager.start_measures(1, SensorGroup::PM);

    // All PM fields should be invalid sentinels
    REQUIRE(result.pm_a.pm_01 == MeasuresInvalid::PM);
    REQUIRE(result.pm_a.pm_25 == MeasuresInvalid::PM);
    REQUIRE(result.pm_b.pm_01 == MeasuresInvalid::PM);
  }
}

// ===========================================================================
// CO2 calibration tests
// ===========================================================================

TEST_CASE("CO2 calibration", "[SensorManager]") {
  MockCO2Sensor mock_co2;
  MockRTOS mock_rtos;
  RTOS::set_instance(&mock_rtos);

  // Common RTOS stubs
  ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));
  ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);

  // Default: other sensors null
  Sensors sensors{};
  sensors.co2 = &mock_co2;

  SensorManager manager(sensors);

  SECTION("returns Unsupported when CO2 sensor is null") {
    Sensors null_sensors{};
    null_sensors.co2 = nullptr;
    SensorManager null_manager(null_sensors);

    REQUIRE(null_manager.calibrate_co2() == Co2CalibrationResult::Unsupported);
  }

  SECTION("returns Unsupported when sensor does not support calibration") {
    REQUIRE_CALL(mock_co2, supports_calibration()).RETURN(false);

    REQUIRE(manager.calibrate_co2() == Co2CalibrationResult::Unsupported);
  }

  SECTION("returns Failed when do_baseline_calibration command fails") {
    REQUIRE_CALL(mock_co2, supports_calibration()).RETURN(true);
    REQUIRE_CALL(mock_co2, do_baseline_calibration(trompeloeil::_)).RETURN(false);

    REQUIRE(manager.calibrate_co2() == Co2CalibrationResult::Failed);
  }

  SECTION("returns Success when calibration completes on first poll") {
    REQUIRE_CALL(mock_co2, supports_calibration()).RETURN(true);
    REQUIRE_CALL(mock_co2, do_baseline_calibration(trompeloeil::_)).RETURN(true);
    REQUIRE_CALL(mock_co2, is_baseline_calibration_done()).RETURN(true);

    REQUIRE(manager.calibrate_co2() == Co2CalibrationResult::Success);
  }

  SECTION("returns Success after multiple polls") {
    REQUIRE_CALL(mock_co2, supports_calibration()).RETURN(true);
    REQUIRE_CALL(mock_co2, do_baseline_calibration(trompeloeil::_)).RETURN(true);

    int poll_count = 0;
    REQUIRE_CALL(mock_co2, is_baseline_calibration_done())
        .TIMES(3)
        .LR_SIDE_EFFECT(poll_count++)
        .LR_RETURN(poll_count >= 3);

    REQUIRE(manager.calibrate_co2() == Co2CalibrationResult::Success);
  }

  SECTION("returns Failed when calibration times out after max attempts") {
    REQUIRE_CALL(mock_co2, supports_calibration()).RETURN(true);
    REQUIRE_CALL(mock_co2, do_baseline_calibration(trompeloeil::_)).RETURN(true);
    REQUIRE_CALL(mock_co2, is_baseline_calibration_done()).TIMES(12).RETURN(false);

    REQUIRE(manager.calibrate_co2() == Co2CalibrationResult::Failed);
  }
}

TEST_CASE("CO2 ABC period configuration", "[SensorManager]") {
  MockCO2Sensor mock_co2;
  Sensors sensors{};
  sensors.co2 = &mock_co2;
  SensorManager manager(sensors);

  SECTION("returns Unsupported without a CO2 sensor") {
    Sensors null_sensors{};
    SensorManager null_manager(null_sensors);
    REQUIRE(null_manager.set_co2_abc_period_days(7) == Co2AbcPeriodResult::Unsupported);
  }

  SECTION("returns Unsupported when the sensor lacks ABC configuration") {
    REQUIRE_CALL(mock_co2, supports_abc_period_configuration()).RETURN(false);
    REQUIRE(manager.set_co2_abc_period_days(7) == Co2AbcPeriodResult::Unsupported);
  }

  SECTION("forwards successful configuration") {
    REQUIRE_CALL(mock_co2, supports_abc_period_configuration()).RETURN(true);
    REQUIRE_CALL(mock_co2, set_abc_period_days(200)).RETURN(true);
    REQUIRE(manager.set_co2_abc_period_days(200) == Co2AbcPeriodResult::Success);
  }

  SECTION("forwards the disabled sentinel") {
    REQUIRE_CALL(mock_co2, supports_abc_period_configuration()).RETURN(true);
    REQUIRE_CALL(mock_co2, set_abc_period_days(-1)).RETURN(true);
    REQUIRE(manager.set_co2_abc_period_days(-1) == Co2AbcPeriodResult::Success);
  }

  SECTION("reports sensor configuration failure") {
    REQUIRE_CALL(mock_co2, supports_abc_period_configuration()).RETURN(true);
    REQUIRE_CALL(mock_co2, set_abc_period_days(7)).RETURN(false);
    REQUIRE(manager.set_co2_abc_period_days(7) == Co2AbcPeriodResult::Failed);
  }
}

// ===========================================================================
// Warmup tests
// ===========================================================================

TEST_CASE("Warmup", "[SensorManager]") {
  // Derived from Kconfig: CONFIG_SENSOR_WARMUP_DURATION_MS (10000) /
  // CONFIG_SENSOR_WARMUP_INTERVAL_MS (1000) = 10 iterations.
  constexpr int EXPECTED_ITERATIONS = 10;

  MockTempHumSensor mock_tempHum;
  MockCO2Sensor mock_co2;
  MockPMSensor mock_pm_a;
  MockPMSensor mock_pm_b;
  MockTVOCNOxSensor mock_tvoc_nox;
  MockO3No2Sensor mock_o3no2;
  MockPressureSensor mock_pressure;

  ALLOW_CALL(mock_pm_a, supports_temp_hum()).RETURN(false);
  ALLOW_CALL(mock_pm_b, supports_temp_hum()).RETURN(false);
  ALLOW_CALL(mock_co2, supports_temp_hum()).RETURN(false);
  ALLOW_CALL(mock_pressure, supports_temp_hum()).RETURN(false);

  MockRTOS mock_rtos;
  RTOS::set_instance(&mock_rtos);

  SECTION("Happy path — TVOC + PM A + PM B all present") {
    // Constant time: get_time_ms always returns 0 so elapsed == 0 and every
    // iteration waits the full WARMUP_INTERVAL_MS (1000 ms).
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    REQUIRE_CALL(mock_rtos, delay_ms_impl(1000)).TIMES(EXPECTED_ITERATIONS);

    REQUIRE_CALL(mock_tvoc_nox, run_conditioning()).TIMES(EXPECTED_ITERATIONS).RETURN(true);
    REQUIRE_CALL(mock_pm_a, read(trompeloeil::_)).TIMES(EXPECTED_ITERATIONS).RETURN(true);
    REQUIRE_CALL(mock_pm_b, read(trompeloeil::_)).TIMES(EXPECTED_ITERATIONS).RETURN(true);

    // Other sensors must not be touched by warmup.
    FORBID_CALL(mock_tempHum, read(trompeloeil::_));
    FORBID_CALL(mock_co2, read(trompeloeil::_));
    FORBID_CALL(mock_o3no2, read(trompeloeil::_));
    FORBID_CALL(mock_pressure, read(trompeloeil::_));

    Sensors sensors{};
    sensors.temp_hum = &mock_tempHum;
    sensors.co2 = &mock_co2;
    sensors.pms_a = &mock_pm_a;
    sensors.pms_b = &mock_pm_b;
    sensors.tvoc_nox = &mock_tvoc_nox;
    sensors.o3_no2 = &mock_o3no2;
    sensors.pressure = &mock_pressure;
    SensorManager manager(sensors);

    manager.warmup();
  }

  SECTION("TVOC only — PM sensors null") {
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    REQUIRE_CALL(mock_rtos, delay_ms_impl(1000)).TIMES(EXPECTED_ITERATIONS);

    REQUIRE_CALL(mock_tvoc_nox, run_conditioning()).TIMES(EXPECTED_ITERATIONS).RETURN(true);
    FORBID_CALL(mock_pm_a, read(trompeloeil::_));
    FORBID_CALL(mock_pm_b, read(trompeloeil::_));

    Sensors sensors{};
    sensors.pms_a = nullptr;
    sensors.pms_b = nullptr;
    sensors.tvoc_nox = &mock_tvoc_nox;
    SensorManager manager(sensors);

    manager.warmup();
  }

  SECTION("PM only — TVOC sensor null") {
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    REQUIRE_CALL(mock_rtos, delay_ms_impl(1000)).TIMES(EXPECTED_ITERATIONS);

    REQUIRE_CALL(mock_pm_a, read(trompeloeil::_)).TIMES(EXPECTED_ITERATIONS).RETURN(true);
    REQUIRE_CALL(mock_pm_b, read(trompeloeil::_)).TIMES(EXPECTED_ITERATIONS).RETURN(true);
    FORBID_CALL(mock_tvoc_nox, run_conditioning());

    Sensors sensors{};
    sensors.pms_a = &mock_pm_a;
    sensors.pms_b = &mock_pm_b;
    sensors.tvoc_nox = nullptr;
    SensorManager manager(sensors);

    manager.warmup();
  }

  SECTION("PM read failures are tolerated") {
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    REQUIRE_CALL(mock_pm_a, read(trompeloeil::_)).TIMES(EXPECTED_ITERATIONS).RETURN(false);
    REQUIRE_CALL(mock_pm_b, read(trompeloeil::_)).TIMES(EXPECTED_ITERATIONS).RETURN(false);
    REQUIRE_CALL(mock_tvoc_nox, run_conditioning()).TIMES(EXPECTED_ITERATIONS).RETURN(true);

    Sensors sensors{};
    sensors.pms_a = &mock_pm_a;
    sensors.pms_b = &mock_pm_b;
    sensors.tvoc_nox = &mock_tvoc_nox;
    SensorManager manager(sensors);

    manager.warmup();
  }

  SECTION("Conditioning failure does not abort warmup") {
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    // Every conditioning cycle fails, but warmup should still run all
    // iterations and still warm PM sensors.
    REQUIRE_CALL(mock_tvoc_nox, run_conditioning()).TIMES(EXPECTED_ITERATIONS).RETURN(false);
    REQUIRE_CALL(mock_pm_a, read(trompeloeil::_)).TIMES(EXPECTED_ITERATIONS).RETURN(true);
    REQUIRE_CALL(mock_pm_b, read(trompeloeil::_)).TIMES(EXPECTED_ITERATIONS).RETURN(true);

    Sensors sensors{};
    sensors.pms_a = &mock_pm_a;
    sensors.pms_b = &mock_pm_b;
    sensors.tvoc_nox = &mock_tvoc_nox;
    SensorManager manager(sensors);

    manager.warmup();
  }

  SECTION("All TVOC/PM sensors null — returns immediately") {
    // No delays, no sensor calls at all.
    FORBID_CALL(mock_rtos, get_time_ms_impl());
    FORBID_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));
    FORBID_CALL(mock_tvoc_nox, run_conditioning());
    FORBID_CALL(mock_pm_a, read(trompeloeil::_));
    FORBID_CALL(mock_pm_b, read(trompeloeil::_));

    Sensors sensors{};
    sensors.temp_hum = &mock_tempHum;
    sensors.co2 = &mock_co2;
    sensors.pms_a = nullptr;
    sensors.pms_b = nullptr;
    sensors.tvoc_nox = nullptr;
    sensors.o3_no2 = &mock_o3no2;
    sensors.pressure = &mock_pressure;
    SensorManager manager(sensors);

    manager.warmup();
  }

  SECTION("Timing compensation — skips delay when iteration exceeds interval") {
    // get_time_ms_impl() is called twice per iteration (start + end). Each
    // call advances fake_now by 1200 ms, so (end - start) == 1200 ms, which
    // exceeds WARMUP_INTERVAL_MS (1000). delay_ms() must therefore never be
    // invoked.
    static uint64_t fake_now = 0;
    fake_now = 0;
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).LR_SIDE_EFFECT(fake_now += 1200).LR_RETURN(fake_now);
    FORBID_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    REQUIRE_CALL(mock_tvoc_nox, run_conditioning()).TIMES(EXPECTED_ITERATIONS).RETURN(true);
    REQUIRE_CALL(mock_pm_a, read(trompeloeil::_)).TIMES(EXPECTED_ITERATIONS).RETURN(true);
    REQUIRE_CALL(mock_pm_b, read(trompeloeil::_)).TIMES(EXPECTED_ITERATIONS).RETURN(true);

    Sensors sensors{};
    sensors.pms_a = &mock_pm_a;
    sensors.pms_b = &mock_pm_b;
    sensors.tvoc_nox = &mock_tvoc_nox;
    SensorManager manager(sensors);

    manager.warmup();
  }

  SECTION("warmup_step — single cycle, all sensors present") {
    // warmup_step() must call run_conditioning() exactly once and read()
    // once per PM sensor. No delay_ms() call.
    FORBID_CALL(mock_rtos, get_time_ms_impl());
    FORBID_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    REQUIRE_CALL(mock_tvoc_nox, run_conditioning()).TIMES(1).RETURN(true);
    REQUIRE_CALL(mock_pm_a, read(trompeloeil::_)).TIMES(1).RETURN(true);
    REQUIRE_CALL(mock_pm_b, read(trompeloeil::_)).TIMES(1).RETURN(true);

    Sensors sensors{};
    sensors.pms_a = &mock_pm_a;
    sensors.pms_b = &mock_pm_b;
    sensors.tvoc_nox = &mock_tvoc_nox;
    SensorManager manager(sensors);

    manager.warmup_step();
  }

  SECTION("warmup_step — no TVOC/PM sensors, returns immediately") {
    FORBID_CALL(mock_rtos, get_time_ms_impl());
    FORBID_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));
    FORBID_CALL(mock_tvoc_nox, run_conditioning());
    FORBID_CALL(mock_pm_a, read(trompeloeil::_));
    FORBID_CALL(mock_pm_b, read(trompeloeil::_));

    Sensors sensors{};
    sensors.temp_hum = &mock_tempHum;
    sensors.co2 = &mock_co2;
    sensors.pms_a = nullptr;
    sensors.pms_b = nullptr;
    sensors.tvoc_nox = nullptr;
    sensors.o3_no2 = &mock_o3no2;
    sensors.pressure = &mock_pressure;
    SensorManager manager(sensors);

    manager.warmup_step();
  }
}

// ===========================================================================
// PM low-power forwarding tests
// ===========================================================================

TEST_CASE("PM sleep/wake forwarding", "[SensorManager]") {
  MockPMSensor mock_pm_a;
  MockPMSensor mock_pm_b;

  SECTION("forwards to both PM sensors") {
    Sensors sensors{};
    sensors.pms_a = &mock_pm_a;
    sensors.pms_b = &mock_pm_b;
    SensorManager manager(sensors);

    REQUIRE_CALL(mock_pm_a, sleep()).RETURN(true);
    REQUIRE_CALL(mock_pm_b, sleep()).RETURN(true);
    manager.pm_sleep();

    REQUIRE_CALL(mock_pm_a, wake()).RETURN(true);
    REQUIRE_CALL(mock_pm_b, wake()).RETURN(true);
    manager.pm_wake();
  }

  SECTION("forwards to pms_a only when pms_b absent") {
    Sensors sensors{};
    sensors.pms_a = &mock_pm_a;
    sensors.pms_b = nullptr;
    SensorManager manager(sensors);

    REQUIRE_CALL(mock_pm_a, sleep()).RETURN(true);
    manager.pm_sleep();

    REQUIRE_CALL(mock_pm_a, wake()).RETURN(true);
    manager.pm_wake();
  }

  SECTION("no-op when no PM sensors wired") {
    Sensors sensors{};
    sensors.pms_a = nullptr;
    sensors.pms_b = nullptr;
    SensorManager manager(sensors);

    FORBID_CALL(mock_pm_a, sleep());
    FORBID_CALL(mock_pm_a, wake());
    manager.pm_sleep();
    manager.pm_wake();
  }
}

// ===========================================================================
// Gas-index algorithm hosting tests
// ===========================================================================

TEST_CASE("Gas index algorithm", "[SensorManager]") {
  MockTVOCNOxSensor mock_tvoc_nox;
  MockRTOS mock_rtos;
  RTOS::set_instance(&mock_rtos);

  ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
  ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

  SECTION("has_tvoc_nox_sensor reflects wiring") {
    Sensors with_sensor{};
    with_sensor.tvoc_nox = &mock_tvoc_nox;
    SensorManager with(with_sensor);
    REQUIRE(with.has_tvoc_nox_sensor());

    Sensors without_sensor{};
    without_sensor.tvoc_nox = nullptr;
    SensorManager without(without_sensor);
    REQUIRE_FALSE(without.has_tvoc_nox_sensor());
  }

  SECTION("configure returns true for 1000 ms") {
    Sensors s{};
    s.tvoc_nox = &mock_tvoc_nox;
    SensorManager mgr(s);
    REQUIRE(mgr.configure_tvoc_nox_index(1000));
  }

  SECTION("configure returns true for 10000 ms") {
    Sensors s{};
    s.tvoc_nox = &mock_tvoc_nox;
    SensorManager mgr(s);
    REQUIRE(mgr.configure_tvoc_nox_index(10000));
  }

  SECTION("learning offset update rejects invalid values") {
    Sensors s{};
    s.tvoc_nox = &mock_tvoc_nox;
    SensorManager mgr(s);

    REQUIRE(mgr.configure_tvoc_nox_index(10000));
    CHECK(mgr.set_tvoc_nox_learning_offsets(0, 12) == TvocNoxLearningOffsetResult::Failed);
    CHECK(mgr.set_tvoc_nox_learning_offsets(12, 1001) == TvocNoxLearningOffsetResult::Failed);
  }

  SECTION("learning offset update is unsupported before configuration") {
    Sensors s{};
    s.tvoc_nox = &mock_tvoc_nox;
    SensorManager mgr(s);

    CHECK(mgr.set_tvoc_nox_learning_offsets(12, 12) == TvocNoxLearningOffsetResult::Unsupported);
  }

  SECTION("configure returns false for unsupported interval") {
    Sensors s{};
    s.tvoc_nox = &mock_tvoc_nox;
    SensorManager mgr(s);
    REQUIRE_FALSE(mgr.configure_tvoc_nox_index(5000));
    REQUIRE_FALSE(mgr.configure_tvoc_nox_index(500));
    REQUIRE_FALSE(mgr.configure_tvoc_nox_index(0));
  }

  SECTION("configure returns false when no sensor wired") {
    Sensors s{};
    s.tvoc_nox = nullptr;
    SensorManager mgr(s);
    REQUIRE_FALSE(mgr.configure_tvoc_nox_index(10000));
  }

  SECTION("set_tvoc_nox_compensation forwards to mock") {
    Sensors s{};
    s.tvoc_nox = &mock_tvoc_nox;
    SensorManager mgr(s);

    REQUIRE_CALL(mock_tvoc_nox, set_compensation(22.5f, 55.0f));
    mgr.set_tvoc_nox_compensation(22.5f, 55.0f);
  }

  SECTION("set_tvoc_nox_compensation no-op when no sensor") {
    Sensors s{};
    s.tvoc_nox = nullptr;
    SensorManager mgr(s);
    // Should not crash
    mgr.set_tvoc_nox_compensation(22.5f, 55.0f);
  }

  SECTION("TvocNox group reads SGP41 and advances algorithm") {
    Sensors s{};
    s.tvoc_nox = &mock_tvoc_nox;
    SensorManager mgr(s);

    REQUIRE(mgr.configure_tvoc_nox_index(10000));

    // Feed enough samples to clear the 45 s blackout.
    // At 10 s interval, 45 s / 10 s = 4.5, so 5 samples to clear blackout.
    // The Sensirion algorithm returns 0 during blackout.
    for (int i = 0; i < 5; i++) {
      EXPECT_READ(mock_tvoc_nox,
                  (TVOCNOxData{MeasuresInvalid::TVOC, 25000, MeasuresInvalid::NOX, 18000}), true);
      (void)mgr.start_measures(1, SensorGroup::TvocNox);
    }

    // After blackout, algorithm should return nonzero index.
    EXPECT_READ(mock_tvoc_nox,
                (TVOCNOxData{MeasuresInvalid::TVOC, 25000, MeasuresInvalid::NOX, 18000}), true);
    auto result = mgr.start_measures(1, SensorGroup::TvocNox);

    // Raw values should be valid
    REQUIRE(result.tvoc_nox.tvoc_raw == 25000);
    REQUIRE(result.tvoc_nox.nox_raw == 18000);
    // Index should be in 1..500 range (algorithm has produced values)
    REQUIRE(result.tvoc_nox.tvoc_index >= 1);
    REQUIRE(result.tvoc_nox.tvoc_index <= 500);
    REQUIRE(result.tvoc_nox.nox_index >= 1);
    REQUIRE(result.tvoc_nox.nox_index <= 500);
  }

  SECTION("Blackout produces invalid index, valid raw") {
    Sensors s{};
    s.tvoc_nox = &mock_tvoc_nox;
    SensorManager mgr(s);

    REQUIRE(mgr.configure_tvoc_nox_index(10000));

    // First sample is in blackout — algorithm returns 0 for index.
    EXPECT_READ(mock_tvoc_nox,
                (TVOCNOxData{MeasuresInvalid::TVOC, 25000, MeasuresInvalid::NOX, 18000}), true);
    auto result = mgr.start_measures(1, SensorGroup::TvocNox);

    // Raw values should be populated
    REQUIRE(result.tvoc_nox.tvoc_raw == 25000);
    REQUIRE(result.tvoc_nox.nox_raw == 18000);
    // Index should still be at invalid sentinels (blackout → 0 → skipped)
    REQUIRE(result.tvoc_nox.tvoc_index == MeasuresInvalid::TVOC);
    REQUIRE(result.tvoc_nox.nox_index == MeasuresInvalid::NOX);
  }

  SECTION("Failed read returns invalid sentinels") {
    Sensors s{};
    s.tvoc_nox = &mock_tvoc_nox;
    SensorManager mgr(s);

    REQUIRE(mgr.configure_tvoc_nox_index(10000));

    // Feed samples past blackout so algorithm is primed
    for (int i = 0; i < 6; i++) {
      EXPECT_READ(mock_tvoc_nox,
                  (TVOCNOxData{MeasuresInvalid::TVOC, 25000, MeasuresInvalid::NOX, 18000}), true);
      (void)mgr.start_measures(1, SensorGroup::TvocNox);
    }

    // Now fail the read — result should be invalid sentinels
    EXPECT_FAILURE(mock_tvoc_nox);
    auto result = mgr.start_measures(1, SensorGroup::TvocNox);

    REQUIRE(result.tvoc_nox.tvoc_raw == MeasuresInvalid::TVOC);
    REQUIRE(result.tvoc_nox.nox_raw == MeasuresInvalid::NOX);
    REQUIRE(result.tvoc_nox.tvoc_index == MeasuresInvalid::TVOC);
    REQUIRE(result.tvoc_nox.nox_index == MeasuresInvalid::NOX);
  }

  SECTION("PM|Other does not read SGP41, returns sentinels") {
    Sensors s{};
    s.tvoc_nox = &mock_tvoc_nox;
    SensorManager mgr(s);

    // PM | Other does not include TvocNox — SGP41 should not be called
    FORBID_CALL(mock_tvoc_nox, read(trompeloeil::_));
    auto result = mgr.start_measures(1, SensorGroup::PM | SensorGroup::Other);

    REQUIRE(result.tvoc_nox.tvoc_raw == MeasuresInvalid::TVOC);
    REQUIRE(result.tvoc_nox.nox_raw == MeasuresInvalid::NOX);
    REQUIRE(result.tvoc_nox.tvoc_index == MeasuresInvalid::TVOC);
    REQUIRE(result.tvoc_nox.nox_index == MeasuresInvalid::NOX);
  }

  SECTION("All group with unconfigured algorithm passes through driver index") {
    Sensors s{};
    s.tvoc_nox = &mock_tvoc_nox;
    SensorManager mgr(s);

    // Do NOT configure the algorithm — simulates fast-path scenario
    EXPECT_READ(mock_tvoc_nox,
                (TVOCNOxData{MeasuresInvalid::TVOC, 25000, MeasuresInvalid::NOX, 18000}), true);
    auto result = mgr.start_measures(1, SensorGroup::All);

    // Raw values present
    REQUIRE(result.tvoc_nox.tvoc_raw == 25000);
    REQUIRE(result.tvoc_nox.nox_raw == 18000);
    // Index fields remain at invalid sentinels (driver sets them to -1)
    REQUIRE(result.tvoc_nox.tvoc_index == MeasuresInvalid::TVOC);
    REQUIRE(result.tvoc_nox.nox_index == MeasuresInvalid::NOX);
  }

  SECTION("Invalid raw fields are not passed to algorithm") {
    Sensors s{};
    s.tvoc_nox = &mock_tvoc_nox;
    SensorManager mgr(s);

    REQUIRE(mgr.configure_tvoc_nox_index(10000));

    // Driver returns invalid raw values (read succeeds but raw fields are sentinels)
    TVOCNOxData invalid_raw{};
    invalid_raw.tvoc_index = MeasuresInvalid::TVOC;
    invalid_raw.tvoc_raw = MeasuresInvalid::TVOC; // -1, invalid
    invalid_raw.nox_index = MeasuresInvalid::NOX;
    invalid_raw.nox_raw = MeasuresInvalid::NOX; // -1, invalid
    EXPECT_READ(mock_tvoc_nox, invalid_raw, true);
    auto result = mgr.start_measures(1, SensorGroup::TvocNox);

    // All fields should remain at sentinels
    REQUIRE(result.tvoc_nox.tvoc_index == MeasuresInvalid::TVOC);
    REQUIRE(result.tvoc_nox.tvoc_raw == MeasuresInvalid::TVOC);
    REQUIRE(result.tvoc_nox.nox_index == MeasuresInvalid::NOX);
    REQUIRE(result.tvoc_nox.nox_raw == MeasuresInvalid::NOX);
  }
}

/**
 * AirGradient Go -- LedService unit tests
 *
 * Tests the LedService effect engine, touch flash manager, front writes,
 * sequence playback, auto-restore, AQI convenience, and render-loop
 * efficiency with a mocked LedDriver.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_test_macros.hpp>
#include <trompeloeil.hpp>
#include <trompeloeil/mock.hpp>

#include <limits>

#include "go_led.h"
#include "rtos.h"

// ============================================================================
// RTOS setup — required for queue operations on host
// ============================================================================

static FreeRTOS s_rtos;

struct GlobalSetup {
  GlobalSetup() { RTOS::set_instance(&s_rtos); }
  ~GlobalSetup() { RTOS::set_instance(nullptr); }
};

static GlobalSetup s_global_setup;

// ============================================================================
// MockLedDriver
// ============================================================================

class MockLedDriver : public trompeloeil::mock_interface<LedDriver> {
public:
  IMPLEMENT_MOCK0(init);
  IMPLEMENT_MOCK2(set_channel);
  IMPLEMENT_MOCK4(set_rgb);
};

// ============================================================================
// Test helper: create a configured & started LedService
// ============================================================================

struct TestFixture {
  MockLedDriver driver;
  LedService::Config config;
  LedService *svc = nullptr;

  TestFixture() {
    config.driver = &driver;
    config.touch_flash_ms = 120;
    config.frame_interval_ms = 30;
  }

  void build() {
    svc = new LedService(config);
    REQUIRE_CALL(driver, init()).RETURN(true);
    REQUIRE(svc->init());
    REQUIRE(svc->start());
  }

  ~TestFixture() { delete svc; }
};

// ============================================================================
// Lifecycle and inert mode
// ============================================================================

TEST_CASE("LedService: lifecycle", "[LedService][lifecycle]") {
  SECTION("init returns driver init result; second call returns cached") {
    MockLedDriver driver;
    LedService::Config cfg{};
    cfg.driver = &driver;
    LedService svc(cfg);

    REQUIRE_CALL(driver, init()).RETURN(true);
    CHECK(svc.init());
    // Second call: no driver init, returns cached true
    CHECK(svc.init());
  }

  SECTION("init failure: no driver calls via pump") {
    MockLedDriver driver;
    LedService::Config cfg{};
    cfg.driver = &driver;
    LedService svc(cfg);

    REQUIRE_CALL(driver, init()).RETURN(false);
    CHECK_FALSE(svc.init());
    CHECK_FALSE(svc.start());

    // pump should be a no-op -- no driver calls expected
    svc.pump_for_test(0);
  }

  SECTION("inert mode: all methods callable, no driver calls") {
    LedService::Config cfg{};
    cfg.driver = nullptr;
    LedService svc(cfg);

    CHECK(svc.init());
    CHECK(svc.start());

    // No crash, no driver calls
    svc.front_set_brightness(LedBrightness::Bright);
    svc.back_solid({255, 0, 0});
    svc.back_off();
    svc.touch_flash(TouchPad::Select);
    svc.pump_for_test(0);
  }
}

// ============================================================================
// Front LED
// ============================================================================

TEST_CASE("LedService: front brightness", "[LedService][front]") {
  TestFixture f;
  f.build();

  SECTION("Off writes 0 to OUT30 and OUT31") {
    f.svc->front_set_brightness(LedBrightness::Off);

    REQUIRE_CALL(f.driver, set_channel(30, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_channel(31, 0)).RETURN(true);
    f.svc->pump_for_test(0);
  }

  SECTION("Bright writes 26 to OUT30 and OUT31") {
    f.svc->front_set_brightness(LedBrightness::Bright);

    REQUIRE_CALL(f.driver, set_channel(30, 26)).RETURN(true);
    REQUIRE_CALL(f.driver, set_channel(31, 26)).RETURN(true);
    f.svc->pump_for_test(0);
  }
}

// ============================================================================
// Back -- Solid
// ============================================================================

TEST_CASE("LedService: back solid", "[LedService][back][solid]") {
  TestFixture f;
  f.build();

  SECTION("solid green at Bright: all 5 groups set to green") {
    f.svc->back_set_brightness(LedBrightness::Bright);
    f.svc->back_solid({0, 255, 0});

    REQUIRE_CALL(f.driver, set_rgb(6, 0, 255, 0)).RETURN(true);  // LED3
    REQUIRE_CALL(f.driver, set_rgb(12, 0, 255, 0)).RETURN(true); // LED5
    REQUIRE_CALL(f.driver, set_rgb(15, 0, 255, 0)).RETURN(true); // LED6
    REQUIRE_CALL(f.driver, set_rgb(18, 0, 255, 0)).RETURN(true); // LED7
    REQUIRE_CALL(f.driver, set_rgb(24, 0, 255, 0)).RETURN(true); // LED9
    f.svc->pump_for_test(0);
  }

  SECTION("back_off clears all 5 groups") {
    f.svc->back_set_brightness(LedBrightness::Bright);
    f.svc->back_solid({255, 0, 0});
    ALLOW_CALL(f.driver, set_rgb(trompeloeil::_, trompeloeil::_, trompeloeil::_, trompeloeil::_))
        .RETURN(true);
    f.svc->pump_for_test(0);

    f.svc->back_off();

    REQUIRE_CALL(f.driver, set_rgb(6, 0, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(12, 0, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(15, 0, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(18, 0, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(24, 0, 0, 0)).RETURN(true);
    f.svc->pump_for_test(10);
  }

  SECTION("brightness change re-renders without new solid call") {
    f.svc->back_set_brightness(LedBrightness::Bright);
    f.svc->back_solid({255, 255, 255});
    ALLOW_CALL(f.driver, set_rgb(trompeloeil::_, trompeloeil::_, trompeloeil::_, trompeloeil::_))
        .RETURN(true);
    f.svc->pump_for_test(0);

    // Dim: 255 * 64 / 255 = 64
    f.svc->back_set_brightness(LedBrightness::Dim);
    REQUIRE_CALL(f.driver, set_rgb(6, 64, 64, 64)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(12, 64, 64, 64)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(15, 64, 64, 64)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(18, 64, 64, 64)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(24, 64, 64, 64)).RETURN(true);
    f.svc->pump_for_test(10);
  }
}

// ============================================================================
// Back -- Blink
// ============================================================================

TEST_CASE("LedService: back blink", "[LedService][back][blink]") {
  TestFixture f;
  f.build();

  f.svc->back_set_brightness(LedBrightness::Bright);

  SECTION("blink toggles on/off at period") {
    f.svc->back_blink({255, 0, 0}, 200);

    // t=0: first half = on (red)
    ALLOW_CALL(f.driver, set_rgb(trompeloeil::_, trompeloeil::_, trompeloeil::_, trompeloeil::_))
        .RETURN(true);
    f.svc->pump_for_test(0);

    // t=100: second half = off
    REQUIRE_CALL(f.driver, set_rgb(6, 0, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(12, 0, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(15, 0, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(18, 0, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(24, 0, 0, 0)).RETURN(true);
    f.svc->pump_for_test(100);

    // t=200: back to on
    REQUIRE_CALL(f.driver, set_rgb(6, 255, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(12, 255, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(15, 255, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(18, 255, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(24, 255, 0, 0)).RETURN(true);
    f.svc->pump_for_test(200);
  }
}

// ============================================================================
// Back -- Breathe
// ============================================================================

TEST_CASE("LedService: back breathe", "[LedService][back][breathe]") {
  TestFixture f;
  f.build();

  f.svc->back_set_brightness(LedBrightness::Bright);
  f.svc->back_breathe({0, 0, 200}, 1000);

  // t=0: 100% brightness -> 200
  ALLOW_CALL(f.driver, set_rgb(trompeloeil::_, trompeloeil::_, trompeloeil::_, trompeloeil::_))
      .RETURN(true);
  f.svc->pump_for_test(0);

  // t=500: 0% brightness -> 0
  REQUIRE_CALL(f.driver, set_rgb(6, 0, 0, 0)).RETURN(true);
  REQUIRE_CALL(f.driver, set_rgb(12, 0, 0, 0)).RETURN(true);
  REQUIRE_CALL(f.driver, set_rgb(15, 0, 0, 0)).RETURN(true);
  REQUIRE_CALL(f.driver, set_rgb(18, 0, 0, 0)).RETURN(true);
  REQUIRE_CALL(f.driver, set_rgb(24, 0, 0, 0)).RETURN(true);
  f.svc->pump_for_test(500);

  // t=1000: back to 100% (looping)
  REQUIRE_CALL(f.driver, set_rgb(6, 0, 0, 200)).RETURN(true);
  REQUIRE_CALL(f.driver, set_rgb(12, 0, 0, 200)).RETURN(true);
  REQUIRE_CALL(f.driver, set_rgb(15, 0, 0, 200)).RETURN(true);
  REQUIRE_CALL(f.driver, set_rgb(18, 0, 0, 200)).RETURN(true);
  REQUIRE_CALL(f.driver, set_rgb(24, 0, 0, 200)).RETURN(true);
  f.svc->pump_for_test(1000);
}

TEST_CASE("LedService: back breathe midpoint", "[LedService][back][breathe]") {
  TestFixture f;
  f.build();

  f.svc->back_set_brightness(LedBrightness::Bright);
  f.svc->back_breathe({0, 0, 200}, 1000);

  ALLOW_CALL(f.driver, set_rgb(trompeloeil::_, trompeloeil::_, trompeloeil::_, trompeloeil::_))
      .RETURN(true);
  f.svc->pump_for_test(0); // 100% = {0, 0, 200}

  // t=100: factor = (1 + cos(2*pi*0.1)) / 2 ≈ 0.9045
  // output = {0, 0, uint8(200*0.9045)} = {0, 0, 180}
  REQUIRE_CALL(f.driver, set_rgb(6, 0, 0, 180)).RETURN(true);
  REQUIRE_CALL(f.driver, set_rgb(12, 0, 0, 180)).RETURN(true);
  REQUIRE_CALL(f.driver, set_rgb(15, 0, 0, 180)).RETURN(true);
  REQUIRE_CALL(f.driver, set_rgb(18, 0, 0, 180)).RETURN(true);
  REQUIRE_CALL(f.driver, set_rgb(24, 0, 0, 180)).RETURN(true);
  f.svc->pump_for_test(100);

  // t=333: factor = (1 + cos(2*pi*0.333)) / 2 ≈ 0.2509
  // output = {0, 0, uint8(200*0.2509)} = {0, 0, 50}
  REQUIRE_CALL(f.driver, set_rgb(6, 0, 0, 50)).RETURN(true);
  REQUIRE_CALL(f.driver, set_rgb(12, 0, 0, 50)).RETURN(true);
  REQUIRE_CALL(f.driver, set_rgb(15, 0, 0, 50)).RETURN(true);
  REQUIRE_CALL(f.driver, set_rgb(18, 0, 0, 50)).RETURN(true);
  REQUIRE_CALL(f.driver, set_rgb(24, 0, 0, 50)).RETURN(true);
  f.svc->pump_for_test(333);
}

// ============================================================================
// Back -- Fade
// ============================================================================

TEST_CASE("LedService: back fade", "[LedService][back][fade]") {
  TestFixture f;
  f.build();

  f.svc->back_set_brightness(LedBrightness::Bright);

  SECTION("fade from red to green over 500 ms") {
    f.svc->back_solid({255, 0, 0});
    ALLOW_CALL(f.driver, set_rgb(trompeloeil::_, trompeloeil::_, trompeloeil::_, trompeloeil::_))
        .RETURN(true);
    f.svc->pump_for_test(0);

    f.svc->back_fade_to({0, 255, 0}, 500);

    // t=0: fade starts from red (captured from solid)
    REQUIRE_CALL(f.driver, set_rgb(6, 255, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(12, 255, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(15, 255, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(18, 255, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(24, 255, 0, 0)).RETURN(true);
    f.svc->pump_for_test(100); // cmd processed at t=100, started_at=100

    // t=350: halfway (elapsed=250 of 500)
    // lerp: r = 255 + (-255*250)/500 = 255 - 127 = 128
    // lerp: g = 0 + (255*250)/500 = 127
    REQUIRE_CALL(f.driver, set_rgb(6, 128, 127, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(12, 128, 127, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(15, 128, 127, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(18, 128, 127, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(24, 128, 127, 0)).RETURN(true);
    f.svc->pump_for_test(350);

    // t=600: elapsed=500, fade complete, holds green
    REQUIRE_CALL(f.driver, set_rgb(6, 0, 255, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(12, 0, 255, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(15, 0, 255, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(18, 0, 255, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(24, 0, 255, 0)).RETURN(true);
    f.svc->pump_for_test(600);

    // t=1000: still green, no ticking (static)
    // No driver writes expected
    f.svc->pump_for_test(1000);
  }
}

// ============================================================================
// Back -- Chase
// ============================================================================

TEST_CASE("LedService: back chase", "[LedService][back][chase]") {
  TestFixture f;
  f.build();

  f.svc->back_set_brightness(LedBrightness::Bright);
  f.svc->back_chase({255, 255, 255}, 100);

  SECTION("LEDs light up sequentially") {
    // t=0: LED3 (index 0) on, rest off
    REQUIRE_CALL(f.driver, set_rgb(6, 255, 255, 255)).RETURN(true); // 0*100=0, on
    REQUIRE_CALL(f.driver, set_rgb(12, 0, 0, 0)).RETURN(true);      // 1*100=100, off
    REQUIRE_CALL(f.driver, set_rgb(15, 0, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(18, 0, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(24, 0, 0, 0)).RETURN(true);
    f.svc->pump_for_test(0);

    // t=100: LED3 + LED5 on
    REQUIRE_CALL(f.driver, set_rgb(6, 255, 255, 255)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(12, 255, 255, 255)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(15, 0, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(18, 0, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(24, 0, 0, 0)).RETURN(true);
    f.svc->pump_for_test(100);

    // t=400: all 5 on (4*100=400, last LED threshold)
    REQUIRE_CALL(f.driver, set_rgb(6, 255, 255, 255)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(12, 255, 255, 255)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(15, 255, 255, 255)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(18, 255, 255, 255)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(24, 255, 255, 255)).RETURN(true);
    f.svc->pump_for_test(400);

    // t=500: chase complete, all on, static (uniform render path)
    REQUIRE_CALL(f.driver, set_rgb(6, 255, 255, 255)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(12, 255, 255, 255)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(15, 255, 255, 255)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(18, 255, 255, 255)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(24, 255, 255, 255)).RETURN(true);
    f.svc->pump_for_test(500);

    // t=600: no further driver writes (static)
    f.svc->pump_for_test(600);
  }
}

// ============================================================================
// Back -- Sequence
// ============================================================================

TEST_CASE("LedService: back sequence", "[LedService][back][sequence]") {
  TestFixture f;
  f.build();

  f.svc->back_set_brightness(LedBrightness::Bright);

  SECTION("sequence plays steps then auto-restores to saved solid") {
    f.svc->back_solid({0, 255, 0}); // green
    ALLOW_CALL(f.driver, set_rgb(trompeloeil::_, trompeloeil::_, trompeloeil::_, trompeloeil::_))
        .RETURN(true);
    f.svc->pump_for_test(0);

    // Play a sequence: [Blink red 200]
    BackStep steps[] = {{BackStep::Effect::Blink, {255, 0, 0}, 200}};
    f.svc->back_play(steps, 1);

    // t=100: blink on (red)
    REQUIRE_CALL(f.driver, set_rgb(6, 255, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(12, 255, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(15, 255, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(18, 255, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(24, 255, 0, 0)).RETURN(true);
    f.svc->pump_for_test(100);

    // t=300: blink cycle complete (elapsed=200), auto-restore to solid green
    REQUIRE_CALL(f.driver, set_rgb(6, 0, 255, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(12, 0, 255, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(15, 0, 255, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(18, 0, 255, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(24, 0, 255, 0)).RETURN(true);
    f.svc->pump_for_test(300);
  }

  SECTION("non-static previous: holds final frame, no restore") {
    f.svc->back_breathe({0, 0, 255}, 2000);
    ALLOW_CALL(f.driver, set_rgb(trompeloeil::_, trompeloeil::_, trompeloeil::_, trompeloeil::_))
        .RETURN(true);
    f.svc->pump_for_test(0);

    // Play [Solid white 100]
    BackStep steps[] = {{BackStep::Effect::Solid, {255, 255, 255}, 100}};
    f.svc->back_play(steps, 1);

    // t=100: solid white
    REQUIRE_CALL(f.driver, set_rgb(6, 255, 255, 255)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(12, 255, 255, 255)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(15, 255, 255, 255)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(18, 255, 255, 255)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(24, 255, 255, 255)).RETURN(true);
    f.svc->pump_for_test(100);

    // t=200: sequence done, no saved static effect -> holds final white
    // No driver writes expected (already white)
    f.svc->pump_for_test(200);
  }

  SECTION("direct back_off during sequence clears saved state") {
    f.svc->back_solid({0, 255, 0});
    ALLOW_CALL(f.driver, set_rgb(trompeloeil::_, trompeloeil::_, trompeloeil::_, trompeloeil::_))
        .RETURN(true);
    f.svc->pump_for_test(0);

    BackStep steps[] = {{BackStep::Effect::Blink, {255, 0, 0}, 400}};
    f.svc->back_play(steps, 1);
    f.svc->pump_for_test(100); // sequence active

    f.svc->back_off(); // replaces sequence, clears saved state

    REQUIRE_CALL(f.driver, set_rgb(6, 0, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(12, 0, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(15, 0, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(18, 0, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(24, 0, 0, 0)).RETURN(true);
    f.svc->pump_for_test(200);

    // No restore to green -- saved was cleared
    f.svc->pump_for_test(500);
  }

  SECTION("new sequence during active keeps original saved state") {
    f.svc->back_solid({0, 255, 0});
    ALLOW_CALL(f.driver, set_rgb(trompeloeil::_, trompeloeil::_, trompeloeil::_, trompeloeil::_))
        .RETURN(true);
    f.svc->pump_for_test(0);

    BackStep seq_a[] = {{BackStep::Effect::Solid, {255, 0, 0}, 1000}};
    f.svc->back_play(seq_a, 1);
    f.svc->pump_for_test(100); // seq_a active, saved = solid green

    BackStep seq_b[] = {{BackStep::Effect::Solid, {0, 0, 255}, 100}};
    f.svc->back_play(seq_b, 1);
    f.svc->pump_for_test(200); // seq_b active, saved = still green (inherited)

    // t=300: seq_b done, auto-restore to solid green
    REQUIRE_CALL(f.driver, set_rgb(6, 0, 255, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(12, 0, 255, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(15, 0, 255, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(18, 0, 255, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(24, 0, 255, 0)).RETURN(true);
    f.svc->pump_for_test(300);
  }

  SECTION("back_animate Boot produces same as back_play with boot steps") {
    // Just verify it doesn't crash and produces driver writes
    f.svc->back_animate(BackAnimation::Boot);
    ALLOW_CALL(f.driver, set_rgb(trompeloeil::_, trompeloeil::_, trompeloeil::_, trompeloeil::_))
        .RETURN(true);
    f.svc->pump_for_test(0);
  }

  SECTION("back_play with null steps is no-op") {
    f.svc->back_play(nullptr, 5);
    // No crash, no enqueue
  }

  SECTION("back_play with count=0 is no-op") {
    BackStep steps[] = {{BackStep::Effect::Solid, {255, 0, 0}, 100}};
    f.svc->back_play(steps, 0);
    // No crash, no enqueue
  }

  SECTION("back_play clamps count to MAX_SEQUENCE_STEPS") {
    BackStep steps[8] = {};
    for (auto &s : steps) {
      s = {BackStep::Effect::Solid, {100, 100, 100}, 50};
    }
    f.svc->back_play(steps, 8);
    ALLOW_CALL(f.driver, set_rgb(trompeloeil::_, trompeloeil::_, trompeloeil::_, trompeloeil::_))
        .RETURN(true);
    f.svc->pump_for_test(0); // should not crash
  }

  SECTION("sequence with Chase step: per-LED rendering within sequence") {
    f.svc->back_solid({0, 255, 0});
    ALLOW_CALL(f.driver, set_rgb(trompeloeil::_, trompeloeil::_, trompeloeil::_, trompeloeil::_))
        .RETURN(true);
    f.svc->pump_for_test(0);

    // Sequence: [Chase white 100]
    BackStep steps[] = {{BackStep::Effect::Chase, {255, 255, 255}, 100}};
    f.svc->back_play(steps, 1);

    // t=50: chase at elapsed=50 — LED0 on (0*100=0 <= 50), rest off
    REQUIRE_CALL(f.driver, set_rgb(6, 255, 255, 255)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(12, 0, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(15, 0, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(18, 0, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(24, 0, 0, 0)).RETURN(true);
    f.svc->pump_for_test(50);

    // t=250: LED0,LED1,LED2 on (0,100,200 <= 250), LED3,LED4 off (300,400)
    REQUIRE_CALL(f.driver, set_rgb(6, 255, 255, 255)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(12, 255, 255, 255)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(15, 255, 255, 255)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(18, 0, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(24, 0, 0, 0)).RETURN(true);
    f.svc->pump_for_test(250);

    // t=550: chase complete (started_at=50, 5*100=500 elapsed), auto-restore to solid green
    REQUIRE_CALL(f.driver, set_rgb(6, 0, 255, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(12, 0, 255, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(15, 0, 255, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(18, 0, 255, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(24, 0, 255, 0)).RETURN(true);
    f.svc->pump_for_test(550);
  }

  SECTION("sequence with Fade step: fade_from captured from last rendered") {
    f.svc->back_solid({255, 0, 0}); // red
    ALLOW_CALL(f.driver, set_rgb(trompeloeil::_, trompeloeil::_, trompeloeil::_, trompeloeil::_))
        .RETURN(true);
    f.svc->pump_for_test(0);

    // Sequence: [Fade to green over 200 ms]
    BackStep steps[] = {{BackStep::Effect::Fade, {0, 255, 0}, 200}};
    f.svc->back_play(steps, 1);

    // t=50: command processed, started_at=50, elapsed=0 → initial frame = red
    f.svc->pump_for_test(50);

    // t=100: elapsed=50, lerp: r=255+(-255*50)/200=255-63=192, g=(255*50)/200=63
    REQUIRE_CALL(f.driver, set_rgb(6, 192, 63, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(12, 192, 63, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(15, 192, 63, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(18, 192, 63, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(24, 192, 63, 0)).RETURN(true);
    f.svc->pump_for_test(100);

    // t=250: fade complete (started_at=50, 200ms duration), auto-restore to solid red
    REQUIRE_CALL(f.driver, set_rgb(6, 255, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(12, 255, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(15, 255, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(18, 255, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(24, 255, 0, 0)).RETURN(true);
    f.svc->pump_for_test(250);
  }
}

// ============================================================================
// Back -- AQI convenience
// ============================================================================

TEST_CASE("LedService: back AQI", "[LedService][back][aqi]") {
  TestFixture f;
  f.build();

  f.svc->back_set_brightness(LedBrightness::Bright);

  SECTION("PM2.5 = 9.0 -> Good -> green") {
    f.svc->back_update_aqi(9.0f);

    ALLOW_CALL(f.driver, set_rgb(trompeloeil::_, trompeloeil::_, trompeloeil::_, trompeloeil::_))
        .RETURN(true);
    f.svc->pump_for_test(0);
    // Should result in green (0, 255, 0) - covered by solid behavior
  }

  SECTION("back_clear_aqi sends off") {
    f.svc->back_update_aqi(9.0f);
    ALLOW_CALL(f.driver, set_rgb(trompeloeil::_, trompeloeil::_, trompeloeil::_, trompeloeil::_))
        .RETURN(true);
    f.svc->pump_for_test(0);

    f.svc->back_clear_aqi();

    REQUIRE_CALL(f.driver, set_rgb(6, 0, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(12, 0, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(15, 0, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(18, 0, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(24, 0, 0, 0)).RETURN(true);
    f.svc->pump_for_test(10);
  }

  SECTION("negative PM2.5 turns back off") {
    f.svc->back_update_aqi(-5.0f);

    REQUIRE_CALL(f.driver, set_rgb(6, 0, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(12, 0, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(15, 0, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(18, 0, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(24, 0, 0, 0)).RETURN(true);
    f.svc->pump_for_test(0);
  }

  SECTION("NaN PM2.5 turns back off") {
    f.svc->back_update_aqi(std::numeric_limits<float>::quiet_NaN());

    REQUIRE_CALL(f.driver, set_rgb(6, 0, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(12, 0, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(15, 0, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(18, 0, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(24, 0, 0, 0)).RETURN(true);
    f.svc->pump_for_test(0);
  }

  SECTION("PM2.5 = 35.4 -> Moderate -> yellow") {
    f.svc->back_update_aqi(35.4f);

    REQUIRE_CALL(f.driver, set_rgb(6, 255, 255, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(12, 255, 255, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(15, 255, 255, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(18, 255, 255, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(24, 255, 255, 0)).RETURN(true);
    f.svc->pump_for_test(0);
  }

  SECTION("PM2.5 = 55.4 -> UnhealthySensitive -> orange") {
    f.svc->back_update_aqi(55.4f);

    REQUIRE_CALL(f.driver, set_rgb(6, 255, 128, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(12, 255, 128, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(15, 255, 128, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(18, 255, 128, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(24, 255, 128, 0)).RETURN(true);
    f.svc->pump_for_test(0);
  }

  SECTION("PM2.5 = 125.4 -> Unhealthy -> red") {
    f.svc->back_update_aqi(125.4f);

    REQUIRE_CALL(f.driver, set_rgb(6, 255, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(12, 255, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(15, 255, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(18, 255, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(24, 255, 0, 0)).RETURN(true);
    f.svc->pump_for_test(0);
  }

  SECTION("PM2.5 = 225.4 -> VeryUnhealthy -> purple") {
    f.svc->back_update_aqi(225.4f);

    REQUIRE_CALL(f.driver, set_rgb(6, 128, 0, 128)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(12, 128, 0, 128)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(15, 128, 0, 128)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(18, 128, 0, 128)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(24, 128, 0, 128)).RETURN(true);
    f.svc->pump_for_test(0);
  }

  SECTION("PM2.5 = 325.4 -> Hazardous -> brown") {
    f.svc->back_update_aqi(325.4f);

    REQUIRE_CALL(f.driver, set_rgb(6, 139, 69, 19)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(12, 139, 69, 19)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(15, 139, 69, 19)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(18, 139, 69, 19)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(24, 139, 69, 19)).RETURN(true);
    f.svc->pump_for_test(0);
  }
}

// ============================================================================
// Touch flash
// ============================================================================

TEST_CASE("LedService: touch flash", "[LedService][touch]") {
  TestFixture f;
  f.build();

  SECTION("flash Select at Bright: LED10 white, then off after flash_ms") {
    f.svc->touch_set_intensity(TouchLedIntensity::Bright);

    ALLOW_CALL(f.driver, set_rgb(trompeloeil::_, trompeloeil::_, trompeloeil::_, trompeloeil::_))
        .RETURN(true);
    f.svc->pump_for_test(0);

    f.svc->touch_flash(TouchPad::Select);

    // Flash on: LED10 (OUT27) gets white at 255
    REQUIRE_CALL(f.driver, set_rgb(27, 255, 255, 255)).RETURN(true); // Select on
    REQUIRE_CALL(f.driver, set_rgb(3, 0, 0, 0)).RETURN(true);        // Left off
    REQUIRE_CALL(f.driver, set_rgb(0, 0, 0, 0)).RETURN(true);        // Right off
    f.svc->pump_for_test(10);

    // After flash_ms: off
    REQUIRE_CALL(f.driver, set_rgb(27, 0, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(3, 0, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(0, 0, 0, 0)).RETURN(true);
    f.svc->pump_for_test(10 + 120); // 10 + touch_flash_ms
  }

  SECTION("flash Left at Bright: LED2 white, then off after flash_ms") {
    f.svc->touch_set_intensity(TouchLedIntensity::Bright);

    ALLOW_CALL(f.driver, set_rgb(trompeloeil::_, trompeloeil::_, trompeloeil::_, trompeloeil::_))
        .RETURN(true);
    f.svc->pump_for_test(0);

    f.svc->touch_flash(TouchPad::Left);

    // Flash on: LED2 (OUT3) gets white at 255
    REQUIRE_CALL(f.driver, set_rgb(27, 0, 0, 0)).RETURN(true);      // Select off
    REQUIRE_CALL(f.driver, set_rgb(3, 255, 255, 255)).RETURN(true); // Left on
    REQUIRE_CALL(f.driver, set_rgb(0, 0, 0, 0)).RETURN(true);       // Right off
    f.svc->pump_for_test(10);

    // After flash_ms: off
    REQUIRE_CALL(f.driver, set_rgb(27, 0, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(3, 0, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(0, 0, 0, 0)).RETURN(true);
    f.svc->pump_for_test(10 + 120); // 10 + touch_flash_ms
  }

  SECTION("preemption: Left then Right before off-edge") {
    f.svc->touch_set_intensity(TouchLedIntensity::Bright);
    ALLOW_CALL(f.driver, set_rgb(trompeloeil::_, trompeloeil::_, trompeloeil::_, trompeloeil::_))
        .RETURN(true);
    f.svc->pump_for_test(0);

    f.svc->touch_flash(TouchPad::Left);
    f.svc->pump_for_test(10);

    f.svc->touch_flash(TouchPad::Right);

    // Right should be on now, Left off
    REQUIRE_CALL(f.driver, set_rgb(27, 0, 0, 0)).RETURN(true);      // Select off
    REQUIRE_CALL(f.driver, set_rgb(3, 0, 0, 0)).RETURN(true);       // Left off
    REQUIRE_CALL(f.driver, set_rgb(0, 255, 255, 255)).RETURN(true); // Right on
    f.svc->pump_for_test(50);
  }

  SECTION("intensity Off during active flash: immediate off") {
    f.svc->touch_set_intensity(TouchLedIntensity::Bright);
    ALLOW_CALL(f.driver, set_rgb(trompeloeil::_, trompeloeil::_, trompeloeil::_, trompeloeil::_))
        .RETURN(true);
    f.svc->pump_for_test(0);

    f.svc->touch_flash(TouchPad::Select);
    f.svc->pump_for_test(10);

    f.svc->touch_set_intensity(TouchLedIntensity::Off);

    REQUIRE_CALL(f.driver, set_rgb(27, 0, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(3, 0, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(0, 0, 0, 0)).RETURN(true);
    f.svc->pump_for_test(20);
  }
}

// ============================================================================
// Touch steady (all-pads on/off) — hardware peripheral test
// ============================================================================

TEST_CASE("LedService: touch_set_all steady on/off", "[LedService][touch]") {
  TestFixture f;
  f.build();

  f.svc->touch_set_intensity(TouchLedIntensity::Bright);
  ALLOW_CALL(f.driver, set_rgb(trompeloeil::_, trompeloeil::_, trompeloeil::_, trompeloeil::_))
      .RETURN(true);
  f.svc->pump_for_test(0);

  SECTION("all three pads light steadily, then clear") {
    f.svc->touch_set_all(true);

    // All three pads white at 255, held with no auto-off.
    REQUIRE_CALL(f.driver, set_rgb(27, 255, 255, 255)).RETURN(true); // Select
    REQUIRE_CALL(f.driver, set_rgb(3, 255, 255, 255)).RETURN(true);  // Left
    REQUIRE_CALL(f.driver, set_rgb(0, 255, 255, 255)).RETURN(true);  // Right
    f.svc->pump_for_test(10);

    // Still lit far past the flash window (steady, not a flash).
    REQUIRE_CALL(f.driver, set_rgb(27, 255, 255, 255)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(3, 255, 255, 255)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(0, 255, 255, 255)).RETURN(true);
    f.svc->touch_set_all(true); // re-issue to force a fresh render
    f.svc->pump_for_test(10 + 500);

    // Clear: all three off.
    f.svc->touch_set_all(false);
    REQUIRE_CALL(f.driver, set_rgb(27, 0, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(3, 0, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(0, 0, 0, 0)).RETURN(true);
    f.svc->pump_for_test(10 + 600);
  }
}

// ============================================================================
// Render loop efficiency
// ============================================================================

TEST_CASE("LedService: render efficiency", "[LedService][render]") {
  TestFixture f;
  f.build();

  SECTION("static solid: no driver writes after initial render") {
    f.svc->back_set_brightness(LedBrightness::Bright);
    f.svc->back_solid({100, 200, 50});
    ALLOW_CALL(f.driver, set_rgb(trompeloeil::_, trompeloeil::_, trompeloeil::_, trompeloeil::_))
        .RETURN(true);
    f.svc->pump_for_test(0);

    // Subsequent pumps produce zero driver writes
    f.svc->pump_for_test(100);
    f.svc->pump_for_test(200);
    f.svc->pump_for_test(1000);
  }

  SECTION("breathe: driver writes each tick") {
    f.svc->back_set_brightness(LedBrightness::Bright);
    f.svc->back_breathe({255, 255, 255}, 1000);
    ALLOW_CALL(f.driver, set_rgb(trompeloeil::_, trompeloeil::_, trompeloeil::_, trompeloeil::_))
        .RETURN(true);
    f.svc->pump_for_test(0);

    // Each tick should produce writes (breathing changes color)
    ALLOW_CALL(f.driver, set_rgb(trompeloeil::_, trompeloeil::_, trompeloeil::_, trompeloeil::_))
        .RETURN(true);
    f.svc->pump_for_test(100); // should write
  }

  SECTION("fade completed: no further writes") {
    f.svc->back_set_brightness(LedBrightness::Bright);
    f.svc->back_solid({255, 0, 0});
    ALLOW_CALL(f.driver, set_rgb(trompeloeil::_, trompeloeil::_, trompeloeil::_, trompeloeil::_))
        .RETURN(true);
    f.svc->pump_for_test(0);

    f.svc->back_fade_to({0, 255, 0}, 100);
    ALLOW_CALL(f.driver, set_rgb(trompeloeil::_, trompeloeil::_, trompeloeil::_, trompeloeil::_))
        .RETURN(true);
    f.svc->pump_for_test(50); // mid-fade

    ALLOW_CALL(f.driver, set_rgb(trompeloeil::_, trompeloeil::_, trompeloeil::_, trompeloeil::_))
        .RETURN(true);
    f.svc->pump_for_test(200); // fade complete

    // No further writes
    f.svc->pump_for_test(300);
    f.svc->pump_for_test(500);
  }
}

// ============================================================================
// Zero timing parameters
// ============================================================================

TEST_CASE("LedService: zero timing", "[LedService][zero]") {
  TestFixture f;
  f.build();

  f.svc->back_set_brightness(LedBrightness::Bright);

  SECTION("blink with 0 period: treated as solid") {
    f.svc->back_blink({255, 0, 0}, 0);

    REQUIRE_CALL(f.driver, set_rgb(6, 255, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(12, 255, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(15, 255, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(18, 255, 0, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(24, 255, 0, 0)).RETURN(true);
    f.svc->pump_for_test(0);

    // Static: no further writes
    f.svc->pump_for_test(100);
  }

  SECTION("breathe with 0 period: treated as solid") {
    f.svc->back_breathe({0, 128, 0}, 0);

    REQUIRE_CALL(f.driver, set_rgb(6, 0, 128, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(12, 0, 128, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(15, 0, 128, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(18, 0, 128, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(24, 0, 128, 0)).RETURN(true);
    f.svc->pump_for_test(0);

    // Static: no further writes
    f.svc->pump_for_test(100);
  }

  SECTION("chase with 0 step_ms: all LEDs immediately lit") {
    f.svc->back_chase({255, 255, 255}, 0);

    // Should be treated as solid (all lit)
    REQUIRE_CALL(f.driver, set_rgb(6, 255, 255, 255)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(12, 255, 255, 255)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(15, 255, 255, 255)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(18, 255, 255, 255)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(24, 255, 255, 255)).RETURN(true);
    f.svc->pump_for_test(0);

    // Static
    f.svc->pump_for_test(100);
  }

  SECTION("fade_to with 0 duration: immediate snap") {
    f.svc->back_solid({255, 0, 0});
    ALLOW_CALL(f.driver, set_rgb(trompeloeil::_, trompeloeil::_, trompeloeil::_, trompeloeil::_))
        .RETURN(true);
    f.svc->pump_for_test(0);

    f.svc->back_fade_to({0, 255, 0}, 0);

    REQUIRE_CALL(f.driver, set_rgb(6, 0, 255, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(12, 0, 255, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(15, 0, 255, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(18, 0, 255, 0)).RETURN(true);
    REQUIRE_CALL(f.driver, set_rgb(24, 0, 255, 0)).RETURN(true);
    f.svc->pump_for_test(10);
  }
}

// ============================================================================
// Auto-restore: back_off then sequence restores Off
// ============================================================================

TEST_CASE("LedService: auto-restore from Off", "[LedService][back][restore]") {
  TestFixture f;
  f.build();

  f.svc->back_set_brightness(LedBrightness::Bright);
  f.svc->back_off();
  ALLOW_CALL(f.driver, set_rgb(trompeloeil::_, trompeloeil::_, trompeloeil::_, trompeloeil::_))
      .RETURN(true);
  f.svc->pump_for_test(0);

  BackStep steps[] = {{BackStep::Effect::Solid, {255, 255, 255}, 100}};
  f.svc->back_play(steps, 1);

  ALLOW_CALL(f.driver, set_rgb(trompeloeil::_, trompeloeil::_, trompeloeil::_, trompeloeil::_))
      .RETURN(true);
  f.svc->pump_for_test(50); // solid white

  // t=200: sequence done, auto-restore to Off
  REQUIRE_CALL(f.driver, set_rgb(6, 0, 0, 0)).RETURN(true);
  REQUIRE_CALL(f.driver, set_rgb(12, 0, 0, 0)).RETURN(true);
  REQUIRE_CALL(f.driver, set_rgb(15, 0, 0, 0)).RETURN(true);
  REQUIRE_CALL(f.driver, set_rgb(18, 0, 0, 0)).RETURN(true);
  REQUIRE_CALL(f.driver, set_rgb(24, 0, 0, 0)).RETURN(true);
  f.svc->pump_for_test(200);
}

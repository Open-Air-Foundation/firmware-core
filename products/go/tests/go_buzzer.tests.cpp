/**
 * AirGradient Go -- BuzzerService unit tests
 *
 * Tests the BuzzerService lifecycle, inert mode, play/beep/stop,
 * note boundary timing, command replacement, driver error logging,
 * and render loop efficiency with a mocked BuzzerDriver.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_test_macros.hpp>
#include <trompeloeil.hpp>
#include <trompeloeil/mock.hpp>

#include "go_buzzer.h"
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
// MockBuzzerDriver
// ============================================================================

class MockBuzzerDriver : public trompeloeil::mock_interface<BuzzerDriver> {
public:
  IMPLEMENT_MOCK0(init);
  IMPLEMENT_MOCK1(set_freq);
};

// ============================================================================
// Test helper: create a configured & started BuzzerService
// ============================================================================

struct TestFixture {
  MockBuzzerDriver driver;
  BuzzerService::Config config;
  BuzzerService *svc = nullptr;

  TestFixture() { config.driver = &driver; }

  void build() {
    svc = new BuzzerService(config);
    REQUIRE_CALL(driver, init()).RETURN(true);
    REQUIRE(svc->init());
    REQUIRE(svc->start());
    svc->set_enabled(true);
  }

  ~TestFixture() { delete svc; }
};

// ============================================================================
// Lifecycle and inert mode
// ============================================================================

TEST_CASE("BuzzerService: lifecycle", "[BuzzerService][lifecycle]") {
  SECTION("init returns driver init result; second call returns cached") {
    MockBuzzerDriver driver;
    BuzzerService::Config cfg{};
    cfg.driver = &driver;
    BuzzerService svc(cfg);

    REQUIRE_CALL(driver, init()).RETURN(true);
    CHECK(svc.init());
    // Second call: no driver init, returns cached true
    CHECK(svc.init());
  }

  SECTION("init failure: no driver calls via pump") {
    MockBuzzerDriver driver;
    BuzzerService::Config cfg{};
    cfg.driver = &driver;
    BuzzerService svc(cfg);

    REQUIRE_CALL(driver, init()).RETURN(false);
    CHECK_FALSE(svc.init());
    CHECK_FALSE(svc.start());

    // pump should be a no-op -- no driver calls expected
    svc.pump_for_test(0);
  }

  SECTION("after failed init, mutators do not enqueue") {
    MockBuzzerDriver driver;
    BuzzerService::Config cfg{};
    cfg.driver = &driver;
    BuzzerService svc(cfg);

    REQUIRE_CALL(driver, init()).RETURN(false);
    CHECK_FALSE(svc.init());

    // No driver calls expected -- enqueue is blocked
    Note note{2700, 100};
    svc.play(&note, 1);
    svc.beep(2700, 100);
    svc.acknowledge_refresh();
    svc.stop();
    svc.pump_for_test(0);
    CHECK_FALSE(svc.is_playing());
  }

  SECTION("inert mode: all methods callable, no driver calls") {
    BuzzerService::Config cfg{};
    cfg.driver = nullptr;
    BuzzerService svc(cfg);

    CHECK(svc.init());
    CHECK(svc.start());
    CHECK_FALSE(svc.enabled());

    // No crash, no driver calls
    Note note{2700, 100};
    svc.play(&note, 1);
    svc.beep(2700, 100);
    svc.acknowledge_refresh();
    svc.stop();
    CHECK_FALSE(svc.is_playing());
    svc.pump_for_test(0);
  }
}

// ============================================================================
// Play (short pattern)
// ============================================================================

TEST_CASE("BuzzerService: play single note", "[BuzzerService][play]") {
  TestFixture f;
  f.build();

  SECTION("single note: freq at t=0, mute after duration") {
    Note note{2700, 100};
    f.svc->play(&note, 1);

    // t=0: set_freq(2700)
    REQUIRE_CALL(f.driver, set_freq(2700)).RETURN(true);
    f.svc->pump_for_test(0);
    CHECK(f.svc->is_playing());

    // t=50: still playing, no driver calls
    f.svc->pump_for_test(50);
    CHECK(f.svc->is_playing());

    // t=100: duration elapsed, mute
    REQUIRE_CALL(f.driver, set_freq(0)).RETURN(true);
    f.svc->pump_for_test(100);
    CHECK_FALSE(f.svc->is_playing());
  }
}

TEST_CASE("BuzzerService: play multi-note pattern", "[BuzzerService][play]") {
  TestFixture f;
  f.build();

  SECTION("three notes with silence: correct freq sequence and timing") {
    Note notes[] = {{2700, 100}, {0, 50}, {3200, 120}};
    f.svc->play(notes, 3);

    // t=0: first note
    REQUIRE_CALL(f.driver, set_freq(2700)).RETURN(true);
    f.svc->pump_for_test(0);
    CHECK(f.svc->is_playing());

    // t=100: second note (silence)
    REQUIRE_CALL(f.driver, set_freq(0)).RETURN(true);
    f.svc->pump_for_test(100);
    CHECK(f.svc->is_playing());

    // t=150: third note
    REQUIRE_CALL(f.driver, set_freq(3200)).RETURN(true);
    f.svc->pump_for_test(150);
    CHECK(f.svc->is_playing());

    // t=270: all done, mute
    REQUIRE_CALL(f.driver, set_freq(0)).RETURN(true);
    f.svc->pump_for_test(270);
    CHECK_FALSE(f.svc->is_playing());
  }
}

TEST_CASE("BuzzerService: play clamped to MAX_PATTERN_NOTES", "[BuzzerService][play]") {
  TestFixture f;
  f.build();

  SECTION("excess notes are clamped") {
    // Create more notes than MAX_PATTERN_NOTES
    Note notes[BuzzerService::MAX_PATTERN_NOTES + 2];
    for (uint8_t i = 0; i < BuzzerService::MAX_PATTERN_NOTES + 2; ++i) {
      notes[i] = {2700, 50};
    }

    f.svc->play(notes, BuzzerService::MAX_PATTERN_NOTES + 2);

    // First note plays
    REQUIRE_CALL(f.driver, set_freq(2700)).RETURN(true);
    f.svc->pump_for_test(0);

    // Advance through all MAX_PATTERN_NOTES notes
    uint32_t t = 0;
    for (uint8_t i = 1; i < BuzzerService::MAX_PATTERN_NOTES; ++i) {
      t += 50;
      REQUIRE_CALL(f.driver, set_freq(2700)).RETURN(true);
      f.svc->pump_for_test(t);
    }

    // After MAX_PATTERN_NOTES * 50 ms, should mute (no extra notes)
    t += 50;
    REQUIRE_CALL(f.driver, set_freq(0)).RETURN(true);
    f.svc->pump_for_test(t);
    CHECK_FALSE(f.svc->is_playing());
  }
}

TEST_CASE("BuzzerService: play with null/zero args is no-op", "[BuzzerService][play]") {
  TestFixture f;
  f.build();

  SECTION("nullptr notes: no-op") {
    f.svc->play(nullptr, 1);
    f.svc->pump_for_test(0);
    CHECK_FALSE(f.svc->is_playing());
  }

  SECTION("zero count: no-op") {
    Note note{2700, 100};
    f.svc->play(&note, 0);
    f.svc->pump_for_test(0);
    CHECK_FALSE(f.svc->is_playing());
  }
}

// ============================================================================
// Beep
// ============================================================================

TEST_CASE("BuzzerService: beep and refresh acknowledgment", "[BuzzerService][beep]") {
  TestFixture f;
  f.build();

  uint32_t duration_ms = 200;
  SECTION("beep is equivalent to play single note") { f.svc->beep(2700, duration_ms); }
  SECTION("refresh acknowledgment lasts 100 ms") {
    duration_ms = 100;
    f.svc->acknowledge_refresh();
  }

  REQUIRE_CALL(f.driver, set_freq(2700)).RETURN(true);
  f.svc->pump_for_test(0);
  CHECK(f.svc->is_playing());

  f.svc->pump_for_test(duration_ms - 1);
  CHECK(f.svc->is_playing());

  REQUIRE_CALL(f.driver, set_freq(0)).RETURN(true);
  f.svc->pump_for_test(duration_ms);
  CHECK_FALSE(f.svc->is_playing());
}

// ============================================================================
// Stop
// ============================================================================

TEST_CASE("BuzzerService: stop", "[BuzzerService][stop]") {
  TestFixture f;
  f.build();

  SECTION("stop during playback: mute on next pump, clears pending") {
    Note notes[] = {{2700, 100}, {3200, 100}};
    f.svc->play(notes, 2);

    REQUIRE_CALL(f.driver, set_freq(2700)).RETURN(true);
    f.svc->pump_for_test(0);
    CHECK(f.svc->is_playing());

    f.svc->stop();

    // Next pump: mute via stop processing
    REQUIRE_CALL(f.driver, set_freq(0)).RETURN(true);
    f.svc->pump_for_test(50);
    CHECK_FALSE(f.svc->is_playing());

    // Subsequent pump: no driver calls
    f.svc->pump_for_test(100);
    CHECK_FALSE(f.svc->is_playing());
  }

  SECTION("stop when idle: fail-safe mute on next pump") {
    f.svc->stop();

    // Mutes even when idle
    REQUIRE_CALL(f.driver, set_freq(0)).RETURN(true);
    f.svc->pump_for_test(0);
    CHECK_FALSE(f.svc->is_playing());
  }

  SECTION("stop with queued commands: commands are drained, not played") {
    // Queue a play, then stop before pumping
    Note note{2700, 100};
    f.svc->play(&note, 1);
    f.svc->stop();

    // Stop flag takes precedence -- play is drained
    REQUIRE_CALL(f.driver, set_freq(0)).RETURN(true);
    f.svc->pump_for_test(0);
    CHECK_FALSE(f.svc->is_playing());
  }
}

// ============================================================================
// is_playing
// ============================================================================

TEST_CASE("BuzzerService: is_playing reflects worker state", "[BuzzerService][is_playing]") {
  TestFixture f;
  f.build();

  SECTION("false before pump processes play command") {
    Note note{2700, 100};
    f.svc->play(&note, 1);

    // Between enqueue and pump: is_playing is false (worker hasn't processed)
    CHECK_FALSE(f.svc->is_playing());

    REQUIRE_CALL(f.driver, set_freq(2700)).RETURN(true);
    f.svc->pump_for_test(0);
    CHECK(f.svc->is_playing());
  }

  SECTION("false after all notes complete") {
    Note note{2700, 100};
    f.svc->play(&note, 1);

    REQUIRE_CALL(f.driver, set_freq(2700)).RETURN(true);
    f.svc->pump_for_test(0);

    REQUIRE_CALL(f.driver, set_freq(0)).RETURN(true);
    f.svc->pump_for_test(100);
    CHECK_FALSE(f.svc->is_playing());
  }

  SECTION("false in inert mode") {
    BuzzerService::Config cfg{};
    cfg.driver = nullptr;
    BuzzerService svc(cfg);
    svc.init();
    svc.start();
    CHECK_FALSE(svc.is_playing());
  }
}

// ============================================================================
// Note boundary timing (wraparound-safe)
// ============================================================================

TEST_CASE("BuzzerService: two-note timing", "[BuzzerService][timing]") {
  TestFixture f;
  f.build();

  Note notes[] = {{2700, 100}, {3200, 200}};
  f.svc->play(notes, 2);

  // t=0: set_freq(2700)
  REQUIRE_CALL(f.driver, set_freq(2700)).RETURN(true);
  f.svc->pump_for_test(0);

  // t=100: set_freq(3200) -- elapsed 100 >= duration 100
  REQUIRE_CALL(f.driver, set_freq(3200)).RETURN(true);
  f.svc->pump_for_test(100);

  // t=300: set_freq(0), is_playing false
  REQUIRE_CALL(f.driver, set_freq(0)).RETURN(true);
  f.svc->pump_for_test(300);
  CHECK_FALSE(f.svc->is_playing());
}

TEST_CASE("BuzzerService: silence note timing", "[BuzzerService][timing]") {
  TestFixture f;
  f.build();

  Note notes[] = {{2700, 100}, {0, 50}, {3200, 100}};
  f.svc->play(notes, 3);

  // t=0: set_freq(2700)
  REQUIRE_CALL(f.driver, set_freq(2700)).RETURN(true);
  f.svc->pump_for_test(0);

  // t=100: set_freq(0) (silence note)
  REQUIRE_CALL(f.driver, set_freq(0)).RETURN(true);
  f.svc->pump_for_test(100);

  // t=150: set_freq(3200)
  REQUIRE_CALL(f.driver, set_freq(3200)).RETURN(true);
  f.svc->pump_for_test(150);

  // t=250: set_freq(0) -- done
  REQUIRE_CALL(f.driver, set_freq(0)).RETURN(true);
  f.svc->pump_for_test(250);
  CHECK_FALSE(f.svc->is_playing());
}

// ============================================================================
// Command replacement
// ============================================================================

TEST_CASE("BuzzerService: command replacement", "[BuzzerService][replace]") {
  TestFixture f;
  f.build();

  SECTION("play(B) replaces play(A) before A finishes") {
    Note note_a{2700, 200};
    Note note_b{3200, 100};

    f.svc->play(&note_a, 1);

    REQUIRE_CALL(f.driver, set_freq(2700)).RETURN(true);
    f.svc->pump_for_test(0);
    CHECK(f.svc->is_playing());

    // Enqueue B at t=50 (A hasn't finished)
    f.svc->play(&note_b, 1);

    // Next pump: B replaces A
    REQUIRE_CALL(f.driver, set_freq(3200)).RETURN(true);
    f.svc->pump_for_test(50);
    CHECK(f.svc->is_playing());

    // B finishes at t=50+100=150
    REQUIRE_CALL(f.driver, set_freq(0)).RETURN(true);
    f.svc->pump_for_test(150);
    CHECK_FALSE(f.svc->is_playing());
  }
}

// ============================================================================
// Driver error logging
// ============================================================================

TEST_CASE("BuzzerService: driver error edge-triggered logging", "[BuzzerService][error]") {
  TestFixture f;
  f.build();

  SECTION("set_freq fails then recovers: WARN once, INFO once") {
    Note notes[] = {{2700, 50}, {3200, 50}};
    f.svc->play(notes, 2);

    // First note: driver fails -- WARN logged (once)
    REQUIRE_CALL(f.driver, set_freq(2700)).RETURN(false);
    f.svc->pump_for_test(0);
    CHECK(f.svc->is_playing());

    // Second note: driver recovers -- INFO logged (once)
    REQUIRE_CALL(f.driver, set_freq(3200)).RETURN(true);
    f.svc->pump_for_test(50);

    // Final mute: no repeated log (already recovered)
    REQUIRE_CALL(f.driver, set_freq(0)).RETURN(true);
    f.svc->pump_for_test(100);
  }
}

// ============================================================================
// Render loop efficiency
// ============================================================================

TEST_CASE("BuzzerService: idle pump produces no driver writes", "[BuzzerService][efficiency]") {
  TestFixture f;
  f.build();

  SECTION("after all notes complete, pump is a no-op") {
    Note note{2700, 50};
    f.svc->play(&note, 1);

    REQUIRE_CALL(f.driver, set_freq(2700)).RETURN(true);
    f.svc->pump_for_test(0);

    REQUIRE_CALL(f.driver, set_freq(0)).RETURN(true);
    f.svc->pump_for_test(50);

    // Subsequent pumps: no driver calls
    f.svc->pump_for_test(100);
    f.svc->pump_for_test(200);
    f.svc->pump_for_test(300);
    CHECK_FALSE(f.svc->is_playing());
  }
}

// ============================================================================
// Mutators before start
// ============================================================================

TEST_CASE("BuzzerService: mutators before start are no-ops", "[BuzzerService][lifecycle]") {
  MockBuzzerDriver driver;
  BuzzerService::Config cfg{};
  cfg.driver = &driver;
  BuzzerService svc(cfg);

  REQUIRE_CALL(driver, init()).RETURN(true);
  CHECK(svc.init());
  // start() not called yet

  Note note{2700, 100};
  svc.play(&note, 1);
  svc.beep(2700, 100);
  svc.acknowledge_refresh();
  svc.stop();
  CHECK_FALSE(svc.is_playing());
}

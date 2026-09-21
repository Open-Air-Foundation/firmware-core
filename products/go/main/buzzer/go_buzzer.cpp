/**
 * AirGradient Go -- Buzzer service implementation
 *
 * Tick-based adaptive worker loop, note sequencing, and command queue.
 * See products/go/specs/buzzer_service.md for the full design.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_buzzer.h"

#include "ag_log.h"

#include <algorithm>

static constexpr const char *TAG = "Buzzer";
static constexpr uint32_t REFRESH_ACK_FREQ_HZ = 2700;
static constexpr uint32_t REFRESH_ACK_DURATION_MS = 100;

// ===========================================================================
// Construction / Destruction
// ===========================================================================

BuzzerService::BuzzerService(const Config &config) : _config(config) {}

BuzzerService::~BuzzerService() {
  if (_task != nullptr) {
    RTOS::task_delete(_task);
  }
  if (_queue != nullptr) {
    RTOS::queue_delete(_queue);
  }
}

// ===========================================================================
// Lifecycle
// ===========================================================================

bool BuzzerService::init() {
  if (_init_called) {
    return _init_ok;
  }
  _init_called = true;

  if (_is_inert()) {
    _init_ok = true;
    return true;
  }

  if (!_config.driver->init()) {
    AG_LOGE(TAG, "driver init failed");
    _init_ok = false;
    return false;
  }

  _queue = RTOS::queue_create(_config.queue_depth, sizeof(Cmd));
  if (_queue == nullptr) {
    AG_LOGE(TAG, "queue_create failed");
    _init_ok = false;
    return false;
  }

  _init_ok = true;
  return true;
}

bool BuzzerService::start() {
  if (_is_inert()) {
    return true;
  }
  if (!_init_ok) {
    return false;
  }
  if (_started) {
    return true;
  }

  // task_create returns false under TEST_HOST (no real thread); that is
  // expected -- tests drive the service via pump_for_test() instead.
  RTOS::task_create(&BuzzerService::_task_entry, "buzzer", _config.task_stack_size, this,
                    _config.task_priority, &_task);

  _started = true;
  return true;
}

// ===========================================================================
// Public API
// ===========================================================================

void BuzzerService::play(const Note *notes, uint8_t count) {
  if (notes == nullptr || count == 0) {
    return;
  }

  Cmd cmd{};
  cmd.kind = Cmd::Kind::Play;
  cmd.note_count = std::min(count, MAX_PATTERN_NOTES);
  for (uint8_t i = 0; i < cmd.note_count; ++i) {
    cmd.notes[i] = notes[i];
  }
  _enqueue(cmd);
}

void BuzzerService::beep(uint32_t freq_hz, uint32_t duration_ms) {
  Note note{freq_hz, duration_ms};
  play(&note, 1);
}

void BuzzerService::acknowledge_refresh() { beep(REFRESH_ACK_FREQ_HZ, REFRESH_ACK_DURATION_MS); }

void BuzzerService::set_enabled(bool enabled) { _enabled = enabled; }

void BuzzerService::stop() {
  _stop_requested.store(true, std::memory_order_release);
  Cmd cmd{};
  cmd.kind = Cmd::Kind::Stop;
  _enqueue(cmd);
}

bool BuzzerService::is_playing() const { return _is_playing_flag.load(std::memory_order_acquire); }

bool BuzzerService::enabled() const { return !_is_inert() && _init_ok; }

// ===========================================================================
// Internal -- Inert mode check
// ===========================================================================

bool BuzzerService::_is_inert() const { return _config.driver == nullptr; }

// ===========================================================================
// Internal -- Enqueue
// ===========================================================================

void BuzzerService::_enqueue(const Cmd &cmd) {
  if (_is_inert() || !_init_ok || !_started) {
    return;
  }
  // Allow Stop commands through even when disabled
  if (!_enabled && cmd.kind != Cmd::Kind::Stop) {
    return;
  }
  RTOS::queue_send(_queue, &cmd, 0);
}

// ===========================================================================
// Internal -- Command processing
// ===========================================================================

void BuzzerService::_process_cmd(const Cmd &cmd, uint32_t now_ms) {
  switch (cmd.kind) {

  case Cmd::Kind::Play:
    _note_count = cmd.note_count;
    for (uint8_t i = 0; i < _note_count; ++i) {
      _notes[i] = cmd.notes[i];
    }
    _current_note = 0;
    _start_note(now_ms);
    break;

  case Cmd::Kind::Stop:
    _stop_playback(now_ms);
    break;
  }
}

// ===========================================================================
// Internal -- Playback helpers
// ===========================================================================

void BuzzerService::_start_note(uint32_t now_ms) {
  if (_current_note >= _note_count) {
    return;
  }

  const Note &note = _notes[_current_note];
  bool ok = _config.driver->set_freq(note.freq_hz);

  // Edge-triggered driver error logging
  if (!ok && !_driver_error_logged) {
    AG_LOGW(TAG, "set_freq failed");
    _driver_error_logged = true;
  } else if (ok && _driver_error_logged) {
    AG_LOGI(TAG, "driver recovered");
    _driver_error_logged = false;
  }

  _note_started_at_ms = now_ms;
  _is_playing_flag.store(true, std::memory_order_release);
}

void BuzzerService::_tick_playback(uint32_t now_ms) {
  if (!_is_playing_flag.load(std::memory_order_acquire)) {
    return;
  }

  if (_current_note >= _note_count) {
    return;
  }

  uint32_t elapsed = now_ms - _note_started_at_ms;
  if (elapsed < _notes[_current_note].duration_ms) {
    return; // Note still playing
  }

  // Advance to next note
  _current_note++;
  if (_current_note >= _note_count) {
    // All notes done -- mute
    bool ok = _config.driver->set_freq(0);
    if (!ok && !_driver_error_logged) {
      AG_LOGW(TAG, "set_freq(0) failed");
      _driver_error_logged = true;
    } else if (ok && _driver_error_logged) {
      AG_LOGI(TAG, "driver recovered");
      _driver_error_logged = false;
    }
    _is_playing_flag.store(false, std::memory_order_release);
    return;
  }

  _start_note(now_ms);
}

void BuzzerService::_stop_playback(uint32_t now_ms) {
  (void)now_ms;

  // Fail-safe mute: always call set_freq(0), even if idle
  bool ok = _config.driver->set_freq(0);
  if (!ok && !_driver_error_logged) {
    AG_LOGW(TAG, "set_freq(0) failed on stop");
    _driver_error_logged = true;
  } else if (ok && _driver_error_logged) {
    AG_LOGI(TAG, "driver recovered");
    _driver_error_logged = false;
  }

  _is_playing_flag.store(false, std::memory_order_release);
  _note_count = 0;
  _current_note = 0;

  _drain_queue();
}

void BuzzerService::_drain_queue() {
  Cmd discard{};
  while (RTOS::queue_receive(_queue, &discard, 0)) {
    // Drop all pending commands
  }
}

// ===========================================================================
// Worker
// ===========================================================================

void BuzzerService::_task_entry(void *arg) { static_cast<BuzzerService *>(arg)->_run(); }

void BuzzerService::_run() {
  while (true) {
    uint32_t now_ms = static_cast<uint32_t>(RTOS::get_time_ms());

    // Check stop flag before processing
    if (_stop_requested.load(std::memory_order_acquire)) {
      _stop_playback(now_ms);
      _stop_requested.store(false, std::memory_order_release);
    }

    // Compute adaptive timeout
    uint32_t timeout_ms = UINT32_MAX; // WAIT_FOREVER

    if (_is_playing_flag.load(std::memory_order_acquire) && _current_note < _note_count) {
      uint32_t elapsed = now_ms - _note_started_at_ms;
      uint32_t duration = _notes[_current_note].duration_ms;
      if (elapsed >= duration) {
        timeout_ms = 0;
      } else {
        timeout_ms = duration - elapsed;
      }
    }

    Cmd cmd{};
    bool got_cmd = RTOS::queue_receive(_queue, &cmd, timeout_ms);

    now_ms = static_cast<uint32_t>(RTOS::get_time_ms());

    // Check stop flag again after waking
    if (_stop_requested.load(std::memory_order_acquire)) {
      _stop_playback(now_ms);
      _stop_requested.store(false, std::memory_order_release);
      continue;
    }

    if (got_cmd) {
      _process_cmd(cmd, now_ms);
    }

    _tick_playback(now_ms);
  }
}

// ===========================================================================
// Test pump -- synchronous driver for host tests
// ===========================================================================

#ifdef TEST_HOST

void BuzzerService::pump_for_test(uint32_t now_ms) {
  if (_is_inert() || !_init_ok) {
    return;
  }

  // Check stop flag
  if (_stop_requested.load(std::memory_order_acquire)) {
    _stop_playback(now_ms);
    _stop_requested.store(false, std::memory_order_release);
    return;
  }

  // Drain all queued commands
  Cmd cmd{};
  while (RTOS::queue_receive(_queue, &cmd, 0)) {
    // Check stop flag before processing each command
    if (_stop_requested.load(std::memory_order_acquire)) {
      _stop_playback(now_ms);
      _stop_requested.store(false, std::memory_order_release);
      return;
    }
    _process_cmd(cmd, now_ms);
  }

  _tick_playback(now_ms);
}

#endif // TEST_HOST

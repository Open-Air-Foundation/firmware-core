/**
 * AirGradient Go -- Buzzer service declaration
 *
 * Drives a magnetic buzzer through an abstract BuzzerDriver surface.
 * Uses a tick-based adaptive worker loop that sleeps when idle and
 * ticks at note boundaries when playing.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

#include "go_buzzer_hal.h"
#include "go_buzzer_types.h"
#include "rtos.h"

#include <atomic>
#include <cstdint>

class BuzzerService {
public:
  static constexpr uint8_t MAX_PATTERN_NOTES = 8;

  struct Config {
    BuzzerDriver *driver = nullptr; ///< nullptr = inert mode
    uint16_t task_stack_size = 2048;
    uint8_t task_priority = 3;
    uint8_t queue_depth = 8;
  };

  explicit BuzzerService(const Config &config);
  ~BuzzerService();

  BuzzerService(const BuzzerService &) = delete;
  BuzzerService &operator=(const BuzzerService &) = delete;

  bool init();
  bool start();

  /// Play a short note sequence (max MAX_PATTERN_NOTES). Fire-and-forget.
  /// Notes are copied into the command. Excess notes are clamped.
  void play(const Note *notes, uint8_t count);

  /// Single tone at given frequency for given duration.
  void beep(uint32_t freq_hz, uint32_t duration_ms);

  /// Acknowledge an accepted shake/refresh request with a short beep.
  void acknowledge_refresh();

  /// Enable or disable sound output. When disabled, sound requests are
  /// silently dropped (stop() still works).
  void set_enabled(bool enabled);

  /// Signal the worker to mute and clear pending notes. Does not
  /// directly access the driver -- takes effect on the next worker
  /// wake, which is immediate because the enqueued Stop command
  /// interrupts queue_receive().
  void stop();

  /// True when the worker is actively playing notes. Thread-safe:
  /// backed by std::atomic<bool>, written by the worker, readable
  /// from any task.
  bool is_playing() const;

  bool enabled() const;

private:
  // -----------------------------------------------------------------------
  // Command struct
  // -----------------------------------------------------------------------

  struct Cmd {
    enum class Kind : uint8_t { Play, Stop };
    Kind kind;

    Note notes[MAX_PATTERN_NOTES]{};
    uint8_t note_count = 0;
  };

  // -----------------------------------------------------------------------
  // Internal helpers
  // -----------------------------------------------------------------------

  bool _is_inert() const;
  void _enqueue(const Cmd &cmd);

  void _process_cmd(const Cmd &cmd, uint32_t now_ms);
  void _tick_playback(uint32_t now_ms);
  void _start_note(uint32_t now_ms);
  void _stop_playback(uint32_t now_ms);
  void _drain_queue();

  // -----------------------------------------------------------------------
  // Worker
  // -----------------------------------------------------------------------

  static void _task_entry(void *arg);
  void _run();

  // -----------------------------------------------------------------------
  // State
  // -----------------------------------------------------------------------

  Config _config;
  bool _enabled = false;

  // Lifecycle
  bool _init_ok = false;
  bool _init_called = false;
  bool _started = false;

  // Worker-owned playback state
  Note _notes[MAX_PATTERN_NOTES]{};
  uint8_t _note_count = 0;
  uint8_t _current_note = 0;
  uint32_t _note_started_at_ms = 0;

  // Thread-safe flags
  std::atomic<bool> _is_playing_flag{false};
  std::atomic<bool> _stop_requested{false};

  // Driver error tracking (edge-triggered logging)
  bool _driver_error_logged = false;

  RtosQueueHandle _queue = nullptr;
  RtosTaskHandle _task = nullptr;

  // -----------------------------------------------------------------------
  // Test access
  // -----------------------------------------------------------------------

#ifdef TEST_HOST
  friend class BuzzerServiceTestAccess;

public:
  void pump_for_test(uint32_t now_ms);
#endif
};

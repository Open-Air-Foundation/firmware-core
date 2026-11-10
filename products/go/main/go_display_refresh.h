#pragma once

#include <cstdint>

enum class RefreshMode : uint8_t {
  Full,    ///< Full GC waveform; rebuilds the basemap.
  Fast,    ///< Full-screen fast waveform.
  Partial, ///< Differential waveform over the body or full canvas.
};

struct DisplayRefreshDecision {
  RefreshMode mode;
  bool full_canvas_partial;
};

// Access while the display framebuffer is owned. The worker records completion
// before releasing it; the next submitter then sees the completed refresh state.
class DisplayRefreshPolicy {
public:
  static constexpr uint8_t DEFAULT_MAX_PARTIAL_OPS = 20;
  static constexpr uint32_t DEFAULT_COOLDOWN_MS = 5000;

  uint8_t differential_ops() const { return _differential_ops; }

  DisplayRefreshDecision home_update(bool header_changed, bool menu_cleanup_due, uint32_t now_ms,
                                     uint8_t max_partial_ops, uint32_t cooldown_ms) const {
    const bool cooling_down =
        _has_expensive_refresh && (now_ms - _last_expensive_refresh_ms) < cooldown_ms;
    RefreshMode mode = RefreshMode::Partial;
    if (!cooling_down) {
      if (_differential_ops >= max_partial_ops) {
        mode = RefreshMode::Full;
      } else if (header_changed || menu_cleanup_due) {
        mode = RefreshMode::Fast;
      }
    }
    // Menu navigation can have deferred header changes even if the latest
    // submitted values match. Include the header while cleanup is pending.
    return {mode, mode == RefreshMode::Partial && (header_changed || menu_cleanup_due)};
  }

  // Called only after a successful waveform has finished, including synchronous
  // initialization. Partial refreshes do not extend the cooldown.
  void completed(RefreshMode mode, uint32_t now_ms) {
    if (mode == RefreshMode::Full) {
      _differential_ops = 0;
    } else if (_differential_ops < UINT8_MAX) {
      ++_differential_ops;
    }
    if (mode != RefreshMode::Partial) {
      _last_expensive_refresh_ms = now_ms;
      _has_expensive_refresh = true;
    }
  }

private:
  uint8_t _differential_ops = 0;
  bool _has_expensive_refresh = false;
  uint32_t _last_expensive_refresh_ms = 0;
};

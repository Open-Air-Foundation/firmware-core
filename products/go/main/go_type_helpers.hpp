/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef GO_TYPE_HELPERS_HPP
#define GO_TYPE_HELPERS_HPP

#include <cstdint>

#include "go_types.h"

// Pure operations on Go types. No storage, clock reads, logging, or hardware access.

constexpr bool tracking_session_active(TrackingState state) {
  return state == TrackingState::Recording || state == TrackingState::Paused;
}

namespace go_type_helpers_detail {

inline constexpr uint64_t MILLISECONDS_PER_SECOND = 1000;

constexpr uint32_t duration_s(uint32_t start_s, uint32_t now_s) {
  if (start_s == TRACKING_TIME_INVALID_S || now_s == TRACKING_TIME_INVALID_S || now_s < start_s) {
    return TRACKING_TIME_INVALID_S;
  }
  return now_s - start_s;
}

} // namespace go_type_helpers_detail

/// Reconcile retained timing with the authoritative tracking state without
/// changing that state. Preserve independently usable elapsed/last-record anchors.
inline TrackingTiming tracking_timing_for_state(TrackingTiming timing, TrackingState state) {
  if (!tracking_session_active(state)) {
    return TrackingTiming{};
  }
  const bool has_segment = timing.recording_started_s != TRACKING_TIME_INVALID_S;
  if (has_segment != (state == TrackingState::Recording)) {
    timing.recording_accumulated_s = TRACKING_TIME_INVALID_S;
    timing.recording_started_s = TRACKING_TIME_INVALID_S;
  }
  return timing;
}

/// Floor retained milliseconds to seconds before narrowing. Unrepresentable time
/// returns TRACKING_TIME_INVALID_S; zero is valid. No clock is read here.
inline uint32_t tracking_seconds_from_ms(uint64_t retained_ms) {
  const uint64_t seconds = retained_ms / go_type_helpers_detail::MILLISECONDS_PER_SECOND;
  return seconds < TRACKING_TIME_INVALID_S ? static_cast<uint32_t>(seconds)
                                           : TRACKING_TIME_INVALID_S;
}

/// Read-only elapsed duration including pauses; unknown/invalid inputs return the sentinel.
inline uint32_t tracking_elapsed_s(const TrackingTiming &timing, uint32_t now_s) {
  return go_type_helpers_detail::duration_s(timing.session_started_s, now_s);
}

/// Sum differences of whole-second anchors, excluding pauses. Subsecond fractions
/// are not retained. Sleep does not end an active segment. Invalid inputs return the sentinel.
inline uint32_t tracking_recording_s(const TrackingTiming &timing, uint32_t now_s) {
  const uint32_t elapsed_s = tracking_elapsed_s(timing, now_s);
  if (elapsed_s == TRACKING_TIME_INVALID_S ||
      timing.recording_accumulated_s == TRACKING_TIME_INVALID_S) {
    return TRACKING_TIME_INVALID_S;
  }

  uint64_t recording_s = timing.recording_accumulated_s;
  if (timing.recording_started_s != TRACKING_TIME_INVALID_S) {
    const uint32_t segment_s =
        go_type_helpers_detail::duration_s(timing.recording_started_s, now_s);
    if (segment_s == TRACKING_TIME_INVALID_S ||
        timing.recording_started_s < timing.session_started_s ||
        timing.recording_accumulated_s > timing.recording_started_s - timing.session_started_s) {
      return TRACKING_TIME_INVALID_S;
    }
    recording_s += segment_s;
  }

  if (recording_s >= TRACKING_TIME_INVALID_S || recording_s > elapsed_s) {
    return TRACKING_TIME_INVALID_S;
  }
  return static_cast<uint32_t>(recording_s);
}

/// Read-only age since the last accepted record; unknown/invalid inputs return the sentinel.
inline uint32_t tracking_last_record_age_s(const TrackingTiming &timing, uint32_t now_s) {
  if (timing.session_started_s == TRACKING_TIME_INVALID_S ||
      timing.last_record_s < timing.session_started_s) {
    return TRACKING_TIME_INVALID_S;
  }
  return go_type_helpers_detail::duration_s(timing.last_record_s, now_s);
}

/// Timing mutations return false without changing the value on invalid input.
/// The caller supplies nondecreasing retained-clock seconds and owns the actual
/// TrackingState. These helpers never decide storage success or route behavior.
/// Start resets all prior timing. Assign TrackingTiming{} to clear it on Stop.
[[nodiscard]] inline bool tracking_timing_start(TrackingTiming &timing, uint32_t now_s) {
  if (now_s == TRACKING_TIME_INVALID_S) {
    return false;
  }

  timing = TrackingTiming{};
  timing.session_started_s = now_s;
  timing.recording_accumulated_s = 0;
  timing.recording_started_s = now_s;
  return true;
}

/// Freeze the active duration. Repeated Pause is a successful no-op.
[[nodiscard]] inline bool tracking_timing_pause(TrackingTiming &timing, uint32_t now_s) {
  const uint32_t recording_s = tracking_recording_s(timing, now_s);
  if (recording_s == TRACKING_TIME_INVALID_S) {
    return false;
  }

  timing.recording_accumulated_s = recording_s;
  timing.recording_started_s = TRACKING_TIME_INVALID_S;
  return true;
}

/// Start another active segment. Repeated Resume preserves the existing anchor.
[[nodiscard]] inline bool tracking_timing_resume(TrackingTiming &timing, uint32_t now_s) {
  if (tracking_recording_s(timing, now_s) == TRACKING_TIME_INVALID_S) {
    return false;
  }

  if (timing.recording_started_s == TRACKING_TIME_INVALID_S) {
    timing.recording_started_s = now_s;
  }
  return true;
}

/// Timestamp a caller-confirmed accepted record while Recording, independently
/// of active-duration validity. The caller supplies the actual tracking state.
[[nodiscard]] inline bool tracking_timing_record_accepted(TrackingTiming &timing,
                                                          TrackingState state, uint32_t now_s) {
  if (state != TrackingState::Recording ||
      tracking_elapsed_s(timing, now_s) == TRACKING_TIME_INVALID_S) {
    return false;
  }
  if (timing.last_record_s != TRACKING_TIME_INVALID_S &&
      tracking_last_record_age_s(timing, now_s) == TRACKING_TIME_INVALID_S) {
    return false;
  }

  timing.last_record_s = now_s;
  return true;
}

#endif // GO_TYPE_HELPERS_HPP

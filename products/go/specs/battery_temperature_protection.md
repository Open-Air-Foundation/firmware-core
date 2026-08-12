# Battery Temperature Protection Progress

Track the implementation of battery temperature protection for AirGradient Go.
Delete this temporary document after all categories are complete and the
evergreen product documentation reflects the shipped behavior.

## Problem

The current firmware does not enforce the required battery operating ranges:
charging is allowed from 0 °C through 45 °C, and discharging is allowed from
-10 °C through 60 °C.

## Goals

- Configure BQ25629 hardware charging and OTG temperature protection.
- Enforce the remaining temperature policy in the interactive and Offline
  fast paths.
- Disable charging, without shutdown, when the NTC reading is invalid.
- Reuse the existing temperature shutdown screen with generic wording.
- Cover the behavior with focused host tests.

## Non-Goals

- Change factory fuel-gauge learning behavior.
- Add thermal-chamber verification.
- Add a separate cold-temperature screen or BLE disconnect reason.
- Change the unused BQ25672/BQ25798 implementation.

## Design

The implementation is divided into the following categories:

1. BQ25629 hardware configuration.
2. Product temperature policy.
3. Interactive and Offline fast-path shutdown handling.
4. Generic temperature shutdown presentation.
5. Focused host tests and documentation updates.

Charge recovery after a temperature or invalid-NTC block uses the 2 °C through
43 °C range. The discharge limits are inclusive; shutdown is requested only
below -10 °C or above 60 °C.

## Implementation Plan

- [x] **Category 1: BQ25629 hardware configuration**
  - [x] Correct the NTC register profile.
  - [x] Apply and verify it during BQ25629 initialization.
  - [x] Fail initialization if the profile cannot be established.
  - [x] Verify the relevant firmware build.
- [x] **Category 2: Product temperature policy**
  - [x] Disable charging outside 0 °C through 45 °C.
  - [x] Disable charging when the NTC reading is invalid.
  - [x] Request temperature shutdown outside -10 °C through 60 °C.
  - [x] Resume charging only from 2 °C through 43 °C after a thermal block.
- [x] **Category 3: Shutdown handling**
  - [x] Honor temperature shutdown in the interactive path.
  - [x] Honor temperature shutdown in the Offline fast path.
- [ ] **Category 4: Generic temperature presentation**
  - Reword the existing temperature shutdown screen for hot or cold trips.
  - Keep the existing BLE temperature shutdown reason.
- [ ] **Category 5: Tests and documentation**
  - [x] Add focused host tests for policy boundaries and both runtime paths.
  - Update existing Go documentation that describes the old thresholds.
  - Review the Go VHUB template; no thermal-chamber test is required.

## Testing Strategy

Use existing native host-test targets. Cover temperatures immediately below,
at, and above each policy boundary, invalid NTC handling, charge recovery, and
temperature shutdown handling in interactive and Offline fast-path operation.

Run the native host-test suite and the Go firmware build after each completed
implementation category.

## Open Questions

None.

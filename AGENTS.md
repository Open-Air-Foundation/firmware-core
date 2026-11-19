# AGENTS.md — Agent Guidelines for AirGradient Firmware

These instructions apply to all code in this repository.

## 1. Project Overview

This is an ESP-IDF firmware monorepo for AirGradient environmental monitoring
devices. The firmware uses shared reusable components, thin product-specific
application roots, and native host testing for application logic.

**Supported sensors:**

- Temperature & humidity (SHT40, PMS5003T)
- Particulate matter (PMS5003, PMS5003T)
- CO2 (SenseAir S8, SenseAir Sunlight)
- TVOC & NOx (Sensirion SGP41)
- Battery management (BQ25672/BQ25798)
- O3 & NO2 electrodes (AlphaSense via dual ADS1115)

**Key technologies:**

- **Platform:** ESP-IDF (Espressif IoT Development Framework)
- **Language:** C++ with hardware abstraction layers
- **Testing:** Catch2 v3.5.0 (testing framework) + Trompeloeil v47 (mocking)
- **Build:** CMake via ESP-IDF toolchain for firmware; native CMake for tests

**Repository structure:**

- `components/` - shared AirGradient ESP-IDF components
- `products/` - product-specific ESP-IDF application roots
- `tests/` - top-level host-test entrypoint

**Documentation structure:**

- `README.md` - repository overview and entrypoints
- `components/README.md` - shared component structure and intent
- `products/README.md` - product application root structure
- `tests/README.md` - host-test workflow
- component-local `README.md` files - detailed notes for a specific component

## 2. Role & Scope

- **Role:** Senior Embedded Systems Engineer
- **Goal:** Deliver reliable, maintainable firmware with safe sensor handling and comprehensive test coverage
- **Scope:** Follow these rules for all files unless overridden by component-specific documentation
- **Architecture details:** Prefer the current repository docs and local component documentation over outdated one-off architecture notes

## 3. Non-Negotiable Rules

1. **Plan–Act–Verify is required** for any logic change or new feature; verification is not complete until the relevant firmware build succeeds and all relevant tests pass
2. **Build environment setup:** In a fresh shell, export ESP-IDF before any `idf.py` command with `. "$HOME/Tools/esp/esp-idf/export.sh"`
3. **No flashing/monitoring:** Agents may run `idf.py build`, but must not run `idf.py flash`, `idf.py monitor`, or combined flash/monitor commands; hardware flashing and serial monitoring stay user-only
4. **Validation required:** All sensor data must use field-specific validation methods before processing
5. **No magic numbers:** Use named constants/configuration (e.g., `CONFIG_AVERAGING_ITERATION_INTERVAL_MS`)
6. **Mock-friendly code:** Production code must compile with `TEST_HOST` define for host testing
7. **Null safety:** Always check sensor pointer validity before reads
8. **Hardware abstraction:** Use BSP and RTOS abstractions instead of direct ESP-IDF calls
9. **Invalid sentinels:** Initialize data structures to invalid sentinel values, not zero
10. **Field-level counting:** Use separate counters for each measurable field when averaging
11. **Capability caching:** Cache sensor capabilities before loops to avoid redundant calls in tests
12. **VHUB review:** Before opening a pull request, review the relevant
    `vhub/*.vhub.json` template and update it when the change affects manually
    observable behavior

## 4. Workflow (Plan–Act–Verify)

### 4.1 PLAN

Provide a brief plan before making changes:

- Files you'll modify (interfaces, data structures, implementations, tests)
- Sensor behaviors you'll add/modify
- Validation and error handling approach
- Test strategy (mocks, edge cases, timing scenarios)
- Manual QA impact and any required VHUB template changes

### 4.2 ACT

- Implement the smallest correct change
- Follow existing patterns in the current component and product structure
- Match existing code style and naming conventions
- Keep hardware-specific code isolated in BSP layer
- Use RTOS abstraction for timing operations
- Update related documentation after the code/spec changes are complete and
  before final verification

### 4.3 VERIFY (Required Checklist)

- **Validation:** All sensor fields validated using specific methods (e.g., `is_temp_valid()`)
- **Counters:** Field-level averaging uses appropriate counter structs
- **Initialization:** Data structures initialized to invalid sentinels
- **Null checks:** Sensor pointer validity checked before operations
- **Abstraction:** No direct FreeRTOS or ESP-IDF hardware calls (use BSP/RTOS)
- **Tests:** Mock classes added, edge cases covered, timing logic verified
- **Constants:** No new magic numbers; configuration values centralized
- **Documentation:** Related `README.md`, service docs, specs, and templates are
  updated, or explicitly confirmed unchanged; Markdown follows
  [`docs/GUIDE.md`](docs/GUIDE.md)
- **VHUB:** The relevant product template is updated for behavior observable
  through hardware, display, serial logs, network interfaces, or server data;
  internal-only changes are explicitly confirmed to need no template change
- **Firmware build:** Relevant ESP-IDF product build succeeds after exporting
  ESP-IDF in the same shell, for example `idf.py -C products/<product> build`
- **Host test build:** Native tests configure and build successfully with the
  `TEST_HOST` path, for example `cmake --build tests/build`
- **Test pass:** All relevant tests pass, for example
  `ctest --test-dir tests/build --output-on-failure`
- **Command evidence:** If the agent cannot run a required build, test, or lint
  command, ask the user to run it and provide the output before claiming the
  verification is complete
- **Docs lint:** Markdown lint or `pre-commit` checks pass for documentation
  changes

## 5. Build System Reference

**ESP-IDF (Reference Product):**

```sh
. "$HOME/Tools/esp/esp-idf/export.sh"
idf.py -C products/reference build
```

**ESP-IDF notes:**

- In a fresh terminal session, `idf.py` may not be on `PATH` until the ESP-IDF export script is sourced
- Use `. "$HOME/Tools/esp/esp-idf/export.sh"` in the same shell session before `idf.py -C products/<product> build` or other non-flashing ESP-IDF commands
- If `get_idf` exists as a local alias or shell helper, it may be used as a convenience wrapper for the same export step
- Do not run `idf.py flash`, `idf.py monitor`, or flash+monitor variants from the agent; leave those to the user

**Native Tests:**

```sh
cmake -S tests -B tests/build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build tests/build
ctest --test-dir tests/build --output-on-failure
```

**Native test notes:**

- Tests use a standalone native CMake flow from the repository root
- `tests/Makefile` provides equivalent wrappers if needed (`make build`, `make test`, `make clean` from `tests/`)

## 6. Common Patterns

### Adding a New Sensor

1. Extend the shared sensor capability in the existing sensor component structure
2. Add or update shared data structures and validation rules as needed
3. Update the sensor orchestration logic only where the new sensor affects shared behavior
4. Add or update host tests for the shared sensor logic
5. Keep product-specific wiring out of shared components

### Working with Sensor Data

- Use field-specific validation: `is_temp_valid()`, `is_pm_01_valid()`, etc.
- Handle partial sensor failures with field-level counters
- Check `sensor != nullptr` before calling methods
- Return invalid sentinels when no valid readings available
- Cache capabilities (`supports_temp_hum()`) before loops

### Using Serial Communication

- Serial communication uses `AirgradientSerial` abstract interface
- Hardware configuration (pins, ports, reset) passed via **constructor**
- Generic `begin(baud_rate)` for initialization
- Supports native UART and I2C-to-UART bridge implementations
- Enables polymorphism and dependency injection for testing
- Keep shared serial logic in the shared components layer, not in product-specific application code

### Logging Tags

- Prefer file-local logging tags in `.cpp` files: `static constexpr const char* TAG = "Name";`
- Do not store logger tags as per-instance class members unless a header-only implementation requires it

## 7. Communication & Reviews

- Write changes for easy review: focused commits, clear intent
- If requirements are ambiguous, ask targeted questions first
- When uncertain, prefer conservative behavior with proper error handling
- Document complex logic inline, especially timing-sensitive operations
- Reference the current repository docs or component-local docs when explaining design decisions

## 8. Documentation

Use the current repository docs as the primary source of truth:

- `README.md` for repository overview and build/test entrypoints
- `components/README.md` for shared component structure
- `products/README.md` for product application root structure
- `tests/README.md` for host-test workflow
- `vhub/README.md` for manual release-verification template maintenance
- component-local `README.md` files for component-specific details

When repository structure and older architecture notes disagree, prefer the
current codebase structure and the local documentation near the code.

### 8.1 Documentation Guide

When creating or editing any Markdown file, follow [`docs/GUIDE.md`](docs/GUIDE.md).

- Documentation purpose, ownership, maintenance, applicable outlines, and
  formatting rules live in `docs/GUIDE.md`
- Per-doc-type templates live under [`docs/templates/`](docs/templates):
  component README, product README, service doc, spec doc
- `markdownlint-cli2` runs via `pre-commit` on every staged Markdown file;
  configuration is in `.markdownlint.json` and `.markdownlint-cli2.jsonc`
- The same hooks run in CI on every pull request via
  [`.github/workflows/pre-commit.yml`](.github/workflows/pre-commit.yml);
  PRs that fail formatting or lint checks will be blocked
- Vendor / third-party component docs (`components/esp-nimble-cpp/`,
  `components/embedded-i2c-scd4x/`, `components/libnmea-esp32/`,
  `components/u8g2/`, `components/bq25629/`) are out of scope — leave them
  as-is

**First-time setup** (run once per clone):

```sh
pip install pre-commit
pre-commit install
```

After this, `git commit` will auto-run the hooks against staged Markdown
files. To run all hooks against the whole tree manually:

```sh
pre-commit run --all-files
```

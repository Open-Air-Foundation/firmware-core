# Host Tests

Native tests for application logic in shared components and products. The top-level
CMake project builds the test dependencies once and registers component and
product executables with CTest. Production sources selected by each test target
compile with `TEST_HOST` and run on the development machine.

## Scope

Use host tests for application-level data validation, calculations, protocol
handling, state transitions, policy, and service orchestration. Exercise this
logic through mocks or fakes at existing hardware and service boundaries.

Concrete hardware drivers are verified on hardware. Do not add driver host tests,
require drivers to compile with `TEST_HOST`, or introduce abstractions, bus mocks,
or driver refactors solely to make drivers host-testable. Existing interfaces
allow application tests to substitute a peripheral without testing its driver.

Host tests do not establish electrical behavior, real RTOS scheduling, radio
performance, or display quality.
Use the [reference product](../products/reference/README.md) for component
hardware smoke tests and the [VHUB workflow](../vhub/README.md) for product
manual verification.

## Layout

| Path | Purpose |
|---|---|
| [`CMakeLists.txt`](CMakeLists.txt) | Native entrypoint and registration of component/product test directories |
| [`Makefile`](Makefile) | Configure, build, run, and clean wrappers |
| `components/<component>/tests/` | Component test sources and target definitions |
| `products/<product>/tests/` | Product test sources and target definitions; Go is currently registered |
| `tests/build/` | Generated native build files, executables, and fetched dependencies |

## Prerequisites

- A native C/C++ toolchain supporting C11 and C++17, CMake 3.16 or newer, Git,
  and a build tool such as Make or Ninja.
- Network access on initial configuration for CMake's pinned Catch2 v3.5.0,
  Trompeloeil v47, and cJSON v1.7.18 dependencies, as required by the targets.
- Initialized Git submodules, including the nested `libnmea` sources used by
  Go GPS tests.
- For the full suite and targets that use TinyCBOR, Go's managed components
  populated through ESP-IDF. OTA and Go tests read TinyCBOR from
  `products/go/managed_components/espressif__cbor/tinycbor/src`.

### Fresh Checkout Setup

Run from the repository root. Use the ESP-IDF version pinned by
[host-test CI](../.github/workflows/host-tests.yml), currently v5.5.6 with the
ESP32-C5 target. Adjust the export-script location to your installation.

```sh
git submodule update --init --recursive
(
  . "$HOME/Tools/esp/esp-idf/export.sh"
  idf.py -C products/go -DIDF_TARGET=esp32c5 reconfigure
)
git diff --exit-code -- products/go/dependencies.lock
```

`reconfigure` resolves managed sources; a firmware build is not needed to
prepare the native suite. The subshell keeps ESP-IDF environment changes out
of the subsequent native build. Investigate unexpected lockfile changes rather
than silently accepting a dependency update. Repeat dependency setup when
manifests or lockfiles change, or managed sources are missing.

## Build

Use a native shell, not an ESP cross-compilation toolchain. From the repository
root:

```sh
cmake -S tests -B tests/build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build tests/build
```

Configuration should report the host C/C++ compilers. Keep this build directory
separate from ESP-IDF product build directories. The equivalent wrapper is:

```sh
make -C tests build
```

### Build Selected Targets

After configuring the aggregate project, build only the executables needed for
iteration. For example:

```sh
cmake --build tests/build --target sensors_tests go_settings_tests
```

Target names come from `add_executable()` in the owning `tests/CMakeLists.txt`.
Building selected targets does not build every executable that the aggregate
CTest tree has registered. Run the selected binaries or the corresponding
CTest subdirectory afterward.

### Standalone Component Build

A component can also be configured separately when its test CMake file contains
a standalone `project()` setup. For example, the sensor suite does not require
Go's managed TinyCBOR sources:

```sh
cmake -S components/airgradient-sensors/tests -B components/airgradient-sensors/tests/build
cmake --build components/airgradient-sensors/tests/build
ctest --test-dir components/airgradient-sensors/tests/build --output-on-failure
```

Check the chosen component's CMake file before using this pattern. Standalone
support and dependency requirements are target-specific; a `tests/` directory
alone does not guarantee independent configuration.

## Run

After building the full aggregate suite:

```sh
ctest --test-dir tests/build --output-on-failure
```

For Make wrappers, `make -C tests test` configures and builds before running
CTest; `make -C tests run` does the same with verbose test output.

### Run Selected Tests

Catch2 binaries accept test names or tags. List available tests before choosing
a filter; an empty selection is not evidence of a pass.

```sh
./tests/build/components_airgradient_sensors_tests/sensors_tests --list-tests
./tests/build/components_airgradient_sensors_tests/sensors_tests "[SensorManager]"
./tests/build/products_go_tests/go_settings_tests "[settings]"
```

To run all registered sensor tests in the aggregate build:

```sh
ctest --test-dir tests/build/components_airgradient_sensors_tests --output-on-failure
```

Use `ctest --test-dir tests/build -N` to inspect registration without running
tests. CTest's `-R` option filters registered test names; Catch2 tags such as
`[settings]` are filters for the executable, not CTest labels.

### Interpret Results

A successful run exits with status zero and reports tests/assertions passing.
Failures name the test and failing assertion. CTest writes details under the
invocation build directory's `Testing/Temporary/LastTest.log`.

If CTest cannot find an executable, build its target first. If configuration
cannot find TinyCBOR or libnmea sources, complete the prerequisite steps. Do
not interpret a skipped test, missing executable, or unmatched filter as
verification of the intended behavior.

## Adding Tests

1. Put tests beside their owner: `components/<component>/tests/` for shared
   behavior or `products/<product>/tests/` for product policy and wiring.
2. Extend an existing executable when appropriate. For a new target, select the
   application logic sources, define `TEST_HOST`, link the needed test libraries, and
   register cases with `catch_discover_tests()`.
3. Add a new test directory to this [CMake entrypoint](CMakeLists.txt) using
   `add_subdirectory()`. Existing registered directories need no second entry.
4. Use mocks or fakes at existing hardware or service boundaries. Test observable
   application behavior, including applicable invalid inputs, partial failures,
   and timing decisions. Keep concrete drivers outside the host-test target.
5. Reconfigure, build, and confirm the expected tests are discovered and pass.
   Update the owning component/product documentation when coverage boundaries
   or test setup change.

For fixtures and target-specific limitations, use the owning README and test
CMake file. [Go's test CMake file](../products/go/tests/CMakeLists.txt) shows
product support libraries and host-only dependencies.

## Clean

```sh
make -C tests clean
```

This removes `tests/build/`, including fetched native dependencies. It does not
remove product managed components or standalone component build directories.

## Continuous Integration

[Host-test CI](../.github/workflows/host-tests.yml) uses Ubuntu 24.04, populates
Go's managed components, checks that the lockfile remains unchanged, then
configures, builds, and runs this native suite. The repository's
[change-aware build policy](../README.md#continuous-integration) determines
when it runs. Firmware compilation and hardware verification remain separate.

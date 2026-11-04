# Host Tests

Top-level entrypoint for the native host-test suite. Builds Catch2 +
Trompeloeil once and aggregates per-component test executables registered
with CTest.

## Layout

| Path | Purpose |
|---|---|
| [`tests/CMakeLists.txt`](CMakeLists.txt) | Top-level CMake entrypoint; fetches Catch2 / Trompeloeil and adds component test subdirectories |
| [`tests/Makefile`](Makefile) | Convenience wrappers: `make build`, `make test`, `make run`, `make clean` |
| `components/<component>/tests/` | Component-owned test source and `CMakeLists.txt` |
| `products/<product>/tests/` | Product-owned test source and `CMakeLists.txt` |

Component-local test directories register their executables back into the
top-level CMake tree via `add_subdirectory()` from
[`tests/CMakeLists.txt`](CMakeLists.txt).

## Build

The host-test CI uses `ubuntu-24.04` to avoid automatic Ubuntu release upgrades
through `ubuntu-latest`.

From the repository root:

```sh
cmake -S tests -B tests/build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build tests/build
```

Or via the wrapper:

```sh
cd tests && make build
```

### Build only one component's tests

Useful when iterating on a single component (only works for components that
own a `tests/CMakeLists.txt`):

```sh
cmake -S components/<component>/tests -B components/<component>/tests/build
cmake --build components/<component>/tests/build
```

## Run

From the repository root:

```sh
ctest --test-dir tests/build --output-on-failure
```

Or via the wrapper:

```sh
cd tests && make test     # plain
cd tests && make run      # verbose (-V)
```

### Run a single executable

Catch2 binaries can be run directly with filters:

```sh
./tests/build/components_airgradient_sensors_tests/sensors_tests --list-tests
./tests/build/components_airgradient_sensors_tests/sensors_tests "[SensorManager]"
./tests/build/components_airgradient_sensors_tests/sensors_tests -s
```

## Clean

```sh
cd tests && make clean
# or
rm -rf tests/build
```

## Adding Component Tests

1. Create `components/<component>/tests/` with a `CMakeLists.txt` that
   defines a Catch2 executable and registers it with `catch_discover_tests`.
2. Add `*.tests.cpp` files for the new behavior.
3. Register the component test directory in
   [`tests/CMakeLists.txt`](CMakeLists.txt) via `add_subdirectory()`.
4. Re-configure (`cmake -S tests -B tests/build`) and verify CTest sees the
   new tests.

For component-specific notes (mocks, fixtures, support libraries), see the
component's own README under `components/<component>/`.

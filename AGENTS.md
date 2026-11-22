# Agent Guidelines

These instructions apply throughout the AirGradient ESP-IDF firmware monorepo.

## Find the Owning Code and Docs

- Start with [README.md](README.md) and the affected component or product README.
  Follow its links to architecture and detailed behavior only as needed.
- Shared capabilities belong in [components/](components/README.md).
  Product policy, wiring, and BSP belong in [products/](products/README.md).
- Check the implementation and tests against the owning documentation. Investigate
  disagreements rather than treating an old spec or an apparent bug as the contract.

## Implementation Boundaries

- Application services use existing hardware and RTOS interfaces. Direct ESP-IDF
  and FreeRTOS calls belong in drivers, BSP, and platform implementations.
- Preserve the established measurement contracts: field-specific validation,
  invalid values for unavailable readings, and separate valid-sample counts.
  See [measurement validity](components/airgradient-common/README.md#measurement-validity)
  and [sensor orchestration](components/airgradient-sensors/README.md).
- Follow the owning component's interface and lifetime contracts, including
  handling absent optional peripherals before accessing them.
- Use named constants for hardware limits, timing, and protocol values; follow
  the owning component's configuration conventions.
- Prefer file-local logging tags in `.cpp` files over per-instance members.

## Testing Boundary

- Host tests cover application logic in shared components and products: policy,
  state transitions, data processing, and service orchestration.
- Use mocks or fakes at existing hardware and service boundaries to test that
  logic. Keep the selected application sources compatible with `TEST_HOST`.
- Do not add host tests for concrete hardware drivers or require drivers to
  compile with `TEST_HOST`. Do not add abstractions, bus mocks, or driver
  refactors solely to make drivers host-testable. Verify drivers on hardware.
- Follow [tests/README.md](tests/README.md) for setup, target selection, and commands.

## Verification

- For production code changes, build the affected firmware products. For
  application logic changes, add or update relevant host tests and run them.
  Run existing affected host tests when changing shared interfaces.
- For test-only changes, build and run the affected host tests. For documentation
  changes, run Markdown checks; documentation-only edits need no firmware build.
- Before any `idf.py` command, source the installed ESP-IDF `export.sh` in the
  same shell. The path in [README.md](README.md#build-firmware) is an example;
  use the actual local installation. Run native tests in a separate native shell.
- Common commands from the repository root, after the applicable setup:

```sh
idf.py -C products/<product> build
cmake -S tests -B tests/build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build tests/build
ctest --test-dir tests/build --output-on-failure
pre-commit run --files <changed-markdown-files>
```

- Report which checks passed and which remain unverified. If a required check
  cannot run, explain the blocker and provide the command needed to complete it;
  do not claim verification is complete.
- Agents must not flash firmware or monitor serial ports. Hardware flashing,
  monitoring, and on-device verification remain user-only.

## Documentation and Manual QA

- Follow [docs/GUIDE.md](docs/GUIDE.md) when editing Markdown. Update the owning
  documentation when behavior or contracts change; link instead of duplicating.
- Leave vendor and third-party documentation unchanged; scope is defined in
  [docs/GUIDE.md](docs/GUIDE.md#scope).
- Before opening a PR, review the relevant [VHUB template](vhub/README.md).
  Update it for changes observable through hardware, display, logs, network
  interfaces, or server data. Internal-only and editorial changes need no cases.

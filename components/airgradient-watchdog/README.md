# airgradient-watchdog

Stateless helpers for feeding an external watchdog through GPIO.

## Status

`Stable`. Extracted from `airgradient-common` with unchanged behavior.

## Scope

Owns pin initialization and the feed pulse. Products own pin selection,
feed intervals, health policy, and feeding during sleep.

## Directory Layout

```text
airgradient-watchdog/
  include/airgradient_watchdog.h
  airgradient_watchdog.cpp
  CMakeLists.txt
  README.md
```

- `include/` — public namespace API
- `airgradient_watchdog.cpp` — GPIO configuration and pulse generation

## Public Includes

```cpp
#include "airgradient_watchdog.h"
```

## Design

```text
product -> watchdog namespace -> GPIO HAL / RTOS
```

`init()` configures an output with floating pulls and disabled interrupts,
returning the GPIO result. `feed()` writes HIGH, delays 20 ms through RTOS,
then writes LOW; it does not report write failures. Calls must be serialized
by the product and run outside ISR context. No object or background task is
created.

## Usage

```cpp
if (!watchdog::init(gpio::native::hal, WATCHDOG_PIN)) {
    return false;
}
watchdog::feed(gpio::native::hal, WATCHDOG_PIN);
```

## Dependencies

- `airgradient-gpio` — public GPIO HAL
- `airgradient-common` — RTOS delay, implementation only

## Tests

Go's power-service [host target](../../products/go/tests/CMakeLists.txt)
links this implementation. Run it through the [host test runner](../../tests/README.md).
The component has no separate test suite.

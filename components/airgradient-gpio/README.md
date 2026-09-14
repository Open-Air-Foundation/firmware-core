# airgradient-gpio

Shared GPIO foundation: a stateless `gpio::Hal` function-pointer table plus
a native ESP-IDF-backed implementation. Covers reusable pin operations only.

## Status

`Stable`.

## Scope

This component owns:

- configuring a pin as input or output
- reading and writing pin level
- configuring interrupt type
- registering and removing GPIO interrupt handlers
- enabling and disabling GPIO interrupts

This component does not own:

- debounce, short-press, long-press, or chord detection
- factory-reset or button-policy logic
- touch input (lives in `airgradient-touch`)

Higher-level button behavior belongs in product-specific code or in a
higher-level service.

## Directory Layout

```text
components/airgradient-gpio/
  hal/
  drivers/
  CMakeLists.txt
  README.md
```

- `hal/` — public GPIO types and the `gpio::Hal` function-pointer table
- `drivers/` — native platform implementation (`gpio::native::hal`), the
  TCA6408A I2C expander driver, and `gpio::expander::hal`, a `gpio::Hal` that
  forwards native pins to the native table and virtual pins
  (`gpio::expander::pin(n)`, numbered from 100) to the attached TCA6408A.
  Expander pins support direction and level only; interrupt registration
  returns `false` so callers fall back to polling.

## Public Includes

```cpp
#include "gpio_hal.h"     // gpio::Hal, gpio::Mode, gpio::PullMode, gpio::InterruptType
#include "native_gpio.h"  // gpio::native::hal (production instance)
```

## Design

GPIO is a global hardware peripheral; no object instance is needed. The
component uses a namespace-based, stateless API:

```text
caller -> const gpio::Hal& -> gpio::native::hal free fns -> ESP-IDF driver/gpio
```

`gpio::Hal` is a plain struct of function pointers — no state, no
initialization, no virtual dispatch. In production, pass `gpio::native::hal`
by const-reference. In tests, construct a `gpio::Hal` directly with mock
function pointers.

## Usage

```cpp
#include "native_gpio.h"

bool init_pin(const gpio::Hal &hal, int pin) {
    return hal.configure(pin, gpio::Mode::Output,
                         gpio::PullMode::Floating,
                         gpio::InterruptType::Disabled);
}

// At the call site:
init_pin(gpio::native::hal, MY_PIN);
```

## Dependencies

- `esp_driver_gpio` — ESP-IDF GPIO driver

## Tests

This component does not currently own host tests. Mock `gpio::Hal`
instances are constructed inline in callers' tests.

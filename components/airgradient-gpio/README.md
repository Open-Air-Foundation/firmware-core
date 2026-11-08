# airgradient-gpio

GPIO HAL for native ESP-IDF pins and TCA6408A I2C expanders.

## Status

`Stable` for the HAL and native backend. TCA6408A hardware validation is
pending; the driver is not wired into a product.

## Scope

Owns pin configuration, reads/writes, native interrupts, and expander access.
Products own bus creation, pin assignments, service wiring, power policy,
and expander INT/RESET wiring. Touch belongs in `airgradient-touch`.

## Directory Layout

```text
components/airgradient-gpio/
  hal/
  drivers/
  CMakeLists.txt
  README.md
```

- `hal/` — GPIO types, operations, HAL, and pin bindings
- `drivers/` — native GPIO and TCA6408A

## Public Includes

```cpp
#include "airgradient_gpio.h" // GPIO types, operations, HAL, pin binding
#include "native_gpio.h"      // gpio::native::hal
#include "tca6408a.h"          // gpio::tca6408a namespace and Context
```

## Design

```text
caller -> gpio::Hal (operations + context) -> backend -> hardware
```

`gpio::Hal` borrows a `gpio::Operations` table and context. Native GPIO uses
null context; TCA6408A uses a caller-owned `gpio::tca6408a::Context`.
`gpio::Pin` copies the binding and adds a pin number, allowing native and
expanded pins together.
The table and context must outlive their bindings. Stop users and call
`deinit(context)` before destroying the context or bus; retry if removal fails.
The context cannot be copied or moved because it contains a mutex.

Operations are task-only and may block. Use one context per device;
its mutex protects complete register updates and waits indefinitely. I2C
transfers have a timeout. Native initialization requires caller serialization.

### TCA6408A Behavior

- `init()` checks communication and preserves pin settings. No chip-ID check
  is possible; the device has no identification register. To change the bus
  or configuration, call `deinit()` before initializing again.
- Supports `Input`/`Output`, `Floating`, and interrupts `Disabled`.
  Unsupported requests fail without register writes.
- Explicit initial levels are loaded before enabling output. Omitting the
  level preserves the latch. Output updates use a shared cache populated at
  initialization: lock, read cache, write hardware, update cache, unlock.
  Unchanged pin values skip the write. Transfer failures invalidate the cache;
  the next partial output update rereads the latch. Port writes update the
  entire latch and cache without changing direction.
- Reads include polarity inversion and acknowledge the shared INT signal,
  even for a single pin. `get_level()` returns `-1` on failure;
  `read_port()` leaves its output unchanged.
- Whole-port writes and polarity settings need coordination between users.
  Configuration failures may leave partial changes. Reinitialize and reapply
  configuration after reset or power loss. All output writes must use this
  context to keep the cache valid.

## Usage

Native callers use `gpio::native::hal`. The board stores an expander context:

```cpp
gpio::tca6408a::Context expander;
```

After bus initialization:

```cpp
if (!gpio::tca6408a::init(expander, bus)) {
    return false;
}
gpio::Pin enable{gpio::tca6408a::hal(expander), ENABLE_PIN};
if (!enable.configure(gpio::Mode::Output, gpio::PullMode::Floating,
                      gpio::InterruptType::Disabled, 0)) {
    return false;
}
return enable.set_level(true);
```

See [`tca6408a.h`](drivers/tca6408a.h) for the full API.

### Driver Configuration

| Field | Default | Purpose |
|---|---|---|
| `address` | `0x20` | Seven-bit address; `0x20` or `0x21` according to ADDR |
| `scl_speed_hz` | `100000` | Positive I2C clock frequency, at most `400000` Hz |
| `timeout_ms` | `100` | Positive timeout for each I2C transaction |

## Dependencies

- `esp_driver_gpio` — native GPIO
- `esp_driver_i2c` — expander I2C
- `airgradient-common` — RTOS mutex abstraction

## Tests

This component has no host tests. Consumers mock `gpio::Hal` in their
[host tests](../../tests/README.md); concrete drivers compile in firmware builds.

## Notes

Hardware reference: [TI TCA6408A datasheet](https://www.ti.com/lit/ds/symlink/tca6408a.pdf).
Bench validation remains pending; firmware builds do not verify hardware.
Product VHUB coverage is unchanged until integration.

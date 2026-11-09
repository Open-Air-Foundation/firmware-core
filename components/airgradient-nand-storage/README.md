# airgradient-nand-storage

SPI NAND filesystem lifecycle: device registration, flash initialization,
and FATFS mount/unmount behind the `NandStorage` interface.

## Status

`Stable`.

## Scope

This component owns the SPI device, NAND driver, filesystem mount, and explicit
format operation. After `init()` succeeds, callers use POSIX file operations
under `mount_path()`.

The caller owns SPI bus initialization, board wiring, file formats, application
synchronization, and flush scheduling. The component does not create a worker.

## Directory Layout

```text
components/airgradient-nand-storage/
  hal/
  drivers/
  CMakeLists.txt
  README.md
```

- `hal/` — `NandStorage` lifecycle interface
- `drivers/` — `SpiNandStorage` implementation and configuration

## Public Includes

```cpp
#include "nand_storage.h"      // Abstract lifecycle interface
#include "spi_nand_storage.h"  // Concrete driver and configuration
```

Depend on `NandStorage` for lifecycle operations; include the concrete driver
when constructing an instance.

## Design

`SpiNandStorage` uses the locally vendored
[`spi_nand_flash`](../spi_nand_flash/) driver, based on version 0.16.0, and
`esp_vfs_fat_nand` for FATFS integration. Application file I/O goes through
ESP-IDF's VFS using standard POSIX calls.

### Initialization And Lifecycle

Initialize the SPI bus with `spi_bus_initialize()` before `init()` and keep it
alive until `deinit()` completes. The storage driver adds and removes its own
SPI device; it does not own the bus.

| Method | Behavior |
|---|---|
| `init()` | Register the device, initialize flash, and mount FATFS; return `false` and clean up on failure. Already mounted is a successful no-op |
| `deinit()` | Unmount and release driver resources; safe after failed initialization. Also called by the destructor |
| `format()` | Require a mounted device, unmount, erase the entire chip, and create/remount FATFS. Failure may leave it unmounted |
| `is_mounted()` | Report whether the filesystem is ready |
| `mount_path()` | Return the configured path; use for file I/O only while mounted |

Lifecycle calls block and are not thread-safe or ISR-safe. Callers must close
files and stop concurrent access before formatting or deinitializing.

### Chip Select

With `cs_gpio == nullptr`, native SPI hardware controls CS. For external CS,
provide a borrowed `gpio::Hal` and configure `cs_pin` as output HIGH before
`init()`. The HAL and its context must outlive the storage instance.

Each external-CS transaction acquires the SPI bus before selecting the device,
then transfers, deselects, and releases the bus. Deselect is attempted even
after selection or transfer errors. The callback runs in task context, allowing
I2C GPIO expanders; ESP-IDF SPI interrupt callbacks are not used.

### Formatting

`format()` erases all stored content and can block for several seconds.
`format_if_mount_failed` separately controls whether initialization may create
a filesystem when mounting fails. It defaults to `true`; callers that need to
preserve existing content should explicitly disable it and handle mount failure.

## Usage

```cpp
// The caller has already initialized SPI2_HOST.
SpiNandStorage nand({
    .spi_host = SPI2_HOST,
    .cs_pin = cs_pin,
    .format_if_mount_failed = false,
});
if (!nand.init()) { return; }
// Use POSIX file I/O under nand.mount_path(); close files before deinit.
nand.deinit();
```

### Driver Configuration

[`SpiNandStorage::Config`](drivers/spi_nand_storage.h) is copied at construction;
the mount-path string is copied into internal storage.

| Field | Default | Purpose |
|---|---|---|
| `spi_host` | `SPI2_HOST` | Caller-initialized SPI bus |
| `cs_pin` | `gpio::INVALID_PIN` | Required CS pin on the selected backend |
| `cs_gpio` | `nullptr` | Borrowed external CS backend; null selects native hardware CS |
| `clock_speed_hz` | `10000000` | SPI clock frequency |
| `spi_device_flags` | `SPI_DEVICE_HALFDUPLEX` | SPI device flags |
| `mount_path` | `/nand` | VFS mount path |
| `format_if_mount_failed` | `true` | Allow automatic formatting after mount failure |
| `max_files` | `4` | Maximum open files |
| `allocation_unit_size` | `16384` | FAT allocation unit in bytes |

## Dependencies

- `spi_nand_flash` — local NAND driver and FATFS integration; Dhara remains managed
- `airgradient-gpio` — optional external CS backend
- `esp_driver_spi` (private) — SPI master driver

## Tests

This component has no automated host tests. Callers can mock `NandStorage`
for lifecycle and failure handling; flash protocol, filesystem mounting, and
external-CS timing require hardware verification. See the
[host-test workflow](../../tests/README.md).

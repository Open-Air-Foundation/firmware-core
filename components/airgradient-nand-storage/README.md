# airgradient-nand-storage

SPI NAND flash HAL: hides the SPI device, NAND flash driver, and FATFS
mount lifecycle behind a clean interface. After `init()` succeeds the
mount path is available for plain POSIX I/O.

## Status

`Stable`.

## Scope

This component owns:

- registering and releasing the SPI device on a caller-initialised bus
- initialising and deinitialising the SPI NAND flash driver
- mounting and unmounting the FATFS filesystem via `esp_vfs_fat_nand`
- exposing the mount path for POSIX I/O by application code
- formatting the volume on demand (`format()`)

This component does not own:

- record formats or file layouts
- application FreeRTOS tasks, queues, or sync policies
- write-back / flush scheduling above the filesystem

All I/O above `init()` is plain POSIX (`fopen`, `fwrite`, `fread`,
`fseek`, `fsync`) with no ESP-IDF involvement.

## Directory Layout

```text
components/airgradient-nand-storage/
  hal/
  drivers/
  CMakeLists.txt
  README.md
```

- `hal/` — `NandStorage` abstract interface
- `drivers/` — `SpiNandStorage` concrete driver with `Config` struct

Chip-select: `Config::cs_pin` uses the SPI driver's hardware CS. When the
board routes NAND CS through an I/O expander (GO v2.0), set `Config::cs_hal`
and `Config::cs_hal_pin` instead; every NAND transaction is then bracketed by
a bus acquire and two HAL writes through the `cs_hook` of the vendored
`components/spi_nand_flash` (see its `AIRGRADIENT_PATCH.md`).

## Public Includes

```cpp
#include "hal/nand_storage.h"
#include "drivers/spi_nand_storage.h"
```

## Design

```text
caller -> NandStorage& -> SpiNandStorage -> spi_nand_flash -> NAND chip
                                         -> esp_vfs_fat_nand -> FATFS at mount_path()
caller <- POSIX file I/O on mount_path() (fopen / fwrite / fread / fsync)
```

`spi_nand_flash` (ESP-IDF managed component) is a private dependency: its
headers are not exposed to callers.

### Prerequisites

The SPI bus must be initialised with `spi_bus_initialize()` before
`init()`. The driver adds and removes its own device handle on the bus.

## Usage

```cpp
SpiNandStorage nand({
    .spi_host               = SPI2_HOST,
    .cs_pin                 = GPIO_NUM_10,
    .mount_path             = "/nand",
    .format_if_mount_failed = true,
});
if (!nand.init()) { /* handle mount failure */ }

// Application I/O — pure POSIX on the mounted path
FILE *f = fopen("/nand/records.bin", "ab");
fwrite(&record, sizeof(record), 1, f);
fclose(f);

nand.deinit();
```

## Dependencies

- `spi_nand_flash` (managed component) — SPI NAND flash driver
- `esp_driver_spi` (private) — SPI master driver

## Tests

This component does not currently own host tests. Storage-dependent
behavior is exercised at the product level via mocked `NandStorage`
references.

# AirGradient patch notes

Vendored copy of `espressif/spi_nand_flash` 0.16.0 (idf-extra-components
`ac7a4e5`), formerly pulled in through
`components/airgradient-nand-storage/idf_component.yml`.

Local change, kept as small as possible so the component can be re-synced:

- `include/spi_nand_flash.h`: `spi_nand_flash_config_t` gains `cs_hook` /
  `cs_hook_ctx`.
- `src/spi_nand_oper.c`: when `cs_hook` is set, each transaction is wrapped in
  `spi_device_acquire_bus()` → `cs_hook(ctx, true)` → `spi_device_transmit()` →
  `cs_hook(ctx, false)` → `spi_device_release_bus()`.

Why: on the GO v2.0 board the NAND chip-select is driven by the TCA6408A I2C
expander, so it cannot be an ESP-IDF hardware CS and cannot be toggled from
the SPI driver's ISR-context callbacks. The hook runs in the calling task and
may block on I2C; holding the bus keeps other SPI devices from clocking the
NAND while its CS is asserted.

Removed from the copy: `examples/`, `test_app/`, `host_test/`,
`CHECKSUMS.json`.

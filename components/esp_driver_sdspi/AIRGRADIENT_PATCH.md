# AirGradient patch notes

Vendored copy of ESP-IDF v5.5.2 `components/esp_driver_sdspi`, placed here so
it **overrides** the core component of the same name. Unlike
`components/spi_nand_flash`, this one shadows a component that ships with the
toolchain: an IDF upgrade will not touch this copy, so it has to be re-synced
by hand and the diff below re-applied.

Local change, kept as small as possible so the component can be re-synced:

- `include/driver/sdspi_host.h`: `sdspi_device_config_t` gains `cs_hook` /
  `cs_hook_ctx`, both NULL in `SDSPI_DEVICE_CONFIG_DEFAULT()`.
- `src/sdspi_host.c`: `slot_info_t` carries the same two fields, `cs_high()`
  and `cs_low()` call the hook when one is set instead of `gpio_set_level()`,
  and the initial deassert moved out of the `gpio_cs` branch so a hooked CS is
  also driven high before the first command.
- `src/sdspi_host.c`: the CS `gpio_config_t` starts with an empty
  `pin_bit_mask` and is filled in inside the `gpio_cs != GPIO_UNUSED` branch.
  Upstream evaluates `1ULL << SDSPI_SLOT_NO_CS` in the initializer — undefined,
  and no longer a mistake now that running without a CS GPIO is a supported
  configuration. The declaration stays at the outer scope because the CD/WP
  block below reuses the same variable.

Why: on the GO v2.0 board the microSD chip-select is pin 6 of the TCA6408A I2C
expander, so it cannot be an ESP-IDF GPIO. Hooking `cs_high()` / `cs_low()` is
enough because upstream already drives CS in software — it sets
`.spics_io_num = GPIO_NUM_NC` and brackets every command with
`spi_device_acquire_bus()`, `cs_low()`, the transfer, `cs_high()`.

Why not wrap `do_transaction` from outside and hold CS low across the call:
`start_command()` clocks 80 cycles **before** asserting CS for CMD0, which is
what puts the card into SPI mode. Holding CS low across the call sends that
preamble with the card selected and it never initializes. The preamble and the
command live in one function, so the two cannot be separated from outside.

Removed from the copy: `test_apps/`.

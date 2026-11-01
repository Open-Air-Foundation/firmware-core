/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef SPI_NAND_STORAGE_H
#define SPI_NAND_STORAGE_H

#include "nand_storage.h"
#include "airgradient_gpio.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "spi_nand_flash.h"

#include <cstddef>

// NandStorage implementation backed by a SPI NAND flash chip and ESP-IDF's
// FATFS VFS layer (esp_vfs_fat_nand).
//
// The SPI bus must be initialised with spi_bus_initialize() by the caller
// before init() is called. This driver adds and removes its own SPI device.
//
// Lifetime of the mount path string: the Config must remain valid for the
// lifetime of the SpiNandStorage object (mount_path is copied internally).
class SpiNandStorage : public NandStorage {
public:
  struct Config {
    // SPI bus. The bus must be initialised by the caller before init().
    spi_host_device_t spi_host = SPI2_HOST;
    gpio_num_t cs_pin = GPIO_NUM_MAX;
    const gpio::Hal *cs_gpio = nullptr; // Optional task-context software CS
    int clock_speed_hz = 10 * 1000 * 1000;
    uint8_t spi_device_flags = SPI_DEVICE_HALFDUPLEX;

    // FATFS mount parameters.
    const char *mount_path = "/nand";
    bool format_if_mount_failed = true;
    int max_files = 4;
    size_t allocation_unit_size = 16 * 1024;
  };

  explicit SpiNandStorage(const Config &config);
  ~SpiNandStorage() override;

  bool init() override;
  void deinit() override;
  bool format() override;
  bool is_mounted() const override;
  const char *mount_path() const override;

private:
  Config _config;
  char _mount_path[64];
  spi_device_handle_t _spi_device{nullptr};
  spi_nand_flash_device_t *_nand_device{nullptr};
  bool _mounted{false};
};

#endif // SPI_NAND_STORAGE_H

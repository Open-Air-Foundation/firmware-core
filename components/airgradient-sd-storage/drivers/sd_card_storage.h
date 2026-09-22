/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef SD_CARD_STORAGE_H
#define SD_CARD_STORAGE_H

#include "airgradient_gpio.h"
#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "sd_protocol_defs.h"
#include "sdmmc_cmd.h"

#include <cstdint>

// Removable microSD card on a shared SPI bus, mounted as FAT through
// esp_vfs_fat_sdspi_mount().
//
// The SPI bus must be initialised with spi_bus_initialize() by the caller
// before init(); this driver registers and removes its own device.
class SdCardStorage {
public:
  struct Config {
    // SPI bus. The bus must be initialised by the caller before init().
    spi_host_device_t spi_host = SPI2_HOST;
    gpio_num_t cs_pin = GPIO_NUM_MAX;

    // Software chip-select: when cs_hal is set, cs_pin is ignored and every
    // card transaction toggles cs_hal_pin through the HAL (v2.0 routes SD CS
    // to the I2C expander).  Costs two HAL writes per transaction.
    const gpio::Hal *cs_hal = nullptr;
    int cs_hal_pin = gpio::INVALID_PIN;

    // Ceiling for the card clock. The host negotiates down from here; probing
    // always runs at 400 kHz regardless.
    int max_freq_khz = 20000;

    const char *mount_path = "/sdcard";
    int max_files = 4;
    size_t allocation_unit_size = 16 * 1024;
  };

  /// What the card reported once it was mounted.
  struct Info {
    char name[8] = {};     ///< product name from CID
    uint64_t capacity_bytes = 0;
    uint32_t speed_khz = 0; ///< clock the host settled on
    uint32_t sector_size = 0;
    bool is_sdhc = false;
  };

  explicit SdCardStorage(const Config &config);
  ~SdCardStorage();

  /// Mounts the card. Never formats: a card that will not mount is reported as
  /// a failure rather than wiped, because it is the user's card and may hold
  /// the only copy of something. format() is explicit and separate.
  bool init();
  void deinit();
  bool is_mounted() const { return _mounted; }
  const char *mount_path() const { return _config.mount_path; }
  bool info(Info &out) const;

  /// Reformats the card as FAT, destroying everything on it.
  bool format();

private:
  static void _cs_hook(void *ctx, bool assert_cs);

  Config _config;
  sdmmc_card_t *_card = nullptr;
  bool _mounted = false;
};

#endif // SD_CARD_STORAGE_H

/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "sd_card_storage.h"

#include "esp_log.h"
#include "esp_vfs_fat.h"

#include <cstring>

static constexpr const char *TAG = "SdCard";

SdCardStorage::SdCardStorage(const Config &config) : _config(config) {}

SdCardStorage::~SdCardStorage() { deinit(); }

void SdCardStorage::_cs_hook(void *ctx, bool assert_cs) {
  auto *self = static_cast<SdCardStorage *>(ctx);
  self->_config.cs_hal->set_level(self->_config.cs_hal_pin, assert_cs ? 0 : 1);
}

bool SdCardStorage::init() {
  if (_mounted) {
    return true;
  }

  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = _config.spi_host;
  host.max_freq_khz = _config.max_freq_khz;

  sdspi_device_config_t dev_config = SDSPI_DEVICE_CONFIG_DEFAULT();
  dev_config.host_id = _config.spi_host;
  if (_config.cs_hal != nullptr) {
    // The card select is not in the GPIO matrix on this board. Tell the host
    // there is no CS pin and drive it ourselves; the hook runs in task context
    // with the SPI bus already held, so it may block on I2C.
    if (_config.cs_hal_pin == gpio::INVALID_PIN ||
        !_config.cs_hal->configure(_config.cs_hal_pin, gpio::Mode::Output, gpio::PullMode::Floating,
                                   gpio::InterruptType::Disabled) ||
        !_config.cs_hal->set_level(_config.cs_hal_pin, 1)) {
      ESP_LOGE(TAG, "software CS pin %d not configurable", _config.cs_hal_pin);
      return false;
    }
    dev_config.gpio_cs = static_cast<gpio_num_t>(SDSPI_SLOT_NO_CS);
    dev_config.cs_hook = &SdCardStorage::_cs_hook;
    dev_config.cs_hook_ctx = this;
  } else {
    dev_config.gpio_cs = _config.cs_pin;
  }

  const esp_vfs_fat_mount_config_t mount_config = {
      .format_if_mount_failed = false,
      .max_files = _config.max_files,
      .allocation_unit_size = _config.allocation_unit_size,
      .disk_status_check_enable = false,
      .use_one_fat = false,
  };
  // The mount helper owns the host: it calls host.init() and registers the
  // SPI device itself, so doing either here would double-initialise it.
  const esp_err_t err =
      esp_vfs_fat_sdspi_mount(_config.mount_path, &host, &dev_config, &mount_config, &_card);
  if (err != ESP_OK) {
    // ESP_FAIL is the filesystem saying no, which is a card that is missing,
    // unformatted, or formatted as something FATFS will not take. Anything
    // else failed before the filesystem was reached.
    ESP_LOGE(TAG, "mount failed: %s%s", esp_err_to_name(err),
             err == ESP_FAIL ? " (no card, or not FAT — format() is explicit)" : "");
    deinit();
    return false;
  }

  _mounted = true;
  Info card{};
  if (info(card)) {
    ESP_LOGI(TAG, "mounted %s: %s %llu MB, %lu kHz, %lu B sectors%s", _config.mount_path, card.name,
             card.capacity_bytes / (1024ULL * 1024ULL), static_cast<unsigned long>(card.speed_khz),
             static_cast<unsigned long>(card.sector_size), card.is_sdhc ? ", SDHC/SDXC" : "");
  }
  return true;
}

void SdCardStorage::deinit() {
  if (_mounted) {
    // Unmounting also tears the host down, mirroring what the mount set up.
    esp_vfs_fat_sdcard_unmount(_config.mount_path, _card);
    _mounted = false;
    _card = nullptr;
  }
  // Leave the card deselected so the next NAND or display transaction on the
  // shared bus is not clocked into it.
  if (_config.cs_hal != nullptr && _config.cs_hal_pin != gpio::INVALID_PIN) {
    _config.cs_hal->set_level(_config.cs_hal_pin, 1);
  }
}

bool SdCardStorage::info(Info &out) const {
  if (!_mounted || _card == nullptr) {
    return false;
  }
  Info card{};
  std::strncpy(card.name, _card->cid.name, sizeof(card.name) - 1);
  card.capacity_bytes =
      static_cast<uint64_t>(_card->csd.capacity) * static_cast<uint64_t>(_card->csd.sector_size);
  card.speed_khz = static_cast<uint32_t>(_card->max_freq_khz);
  card.sector_size = static_cast<uint32_t>(_card->csd.sector_size);
  card.is_sdhc = _card->ocr & SD_OCR_SDHC_CAP;
  out = card;
  return true;
}

bool SdCardStorage::format() {
  if (!_mounted || _card == nullptr) {
    ESP_LOGE(TAG, "format requires a mounted card");
    return false;
  }
  const esp_err_t err = esp_vfs_fat_sdcard_format(_config.mount_path, _card);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "format failed: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

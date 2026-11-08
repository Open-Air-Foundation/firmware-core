/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "spi_nand_storage.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_vfs_fat_nand.h"

static constexpr const char *TAG = "SpiNandStorage";

SpiNandStorage::SpiNandStorage(const Config &config) : _config(config) {
  snprintf(_mount_path, sizeof(_mount_path), "%s", config.mount_path ? config.mount_path : "/nand");
}

SpiNandStorage::~SpiNandStorage() { deinit(); }

esp_err_t SpiNandStorage::set_cs(void *context, bool high) {
  const auto &config = static_cast<SpiNandStorage *>(context)->_config;
  if (!config.cs_gpio->set_level(config.cs_pin, high ? 1 : 0)) {
    return ESP_FAIL;
  }
  return ESP_OK;
}

bool SpiNandStorage::init() {
  if (_mounted) {
    return true;
  }

  if (_config.cs_pin < 0 || (_config.cs_gpio == nullptr && _config.cs_pin >= GPIO_NUM_MAX)) {
    ESP_LOGE(TAG, "cs_pin not configured");
    return false;
  }

  // Register SPI device on the pre-initialised bus.
  spi_device_interface_config_t dev_cfg = {};
  dev_cfg.mode = 0;
  dev_cfg.clock_source = SPI_CLK_SRC_DEFAULT;
  dev_cfg.duty_cycle_pos = 128;
  dev_cfg.clock_speed_hz = _config.clock_speed_hz;
  dev_cfg.spics_io_num = _config.cs_gpio != nullptr ? -1 : _config.cs_pin;
  dev_cfg.flags = _config.spi_device_flags;
  dev_cfg.queue_size = 10;

  esp_err_t err = spi_bus_add_device(_config.spi_host, &dev_cfg, &_spi_device);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
    return false;
  }

  // Initialise the NAND flash device.
  spi_nand_flash_config_t nand_cfg = {};
  nand_cfg.device_handle = _spi_device;
  nand_cfg.gc_factor = 4;
  nand_cfg.io_mode = SPI_NAND_IO_MODE_SIO;
  nand_cfg.flags = _config.spi_device_flags;
  if (_config.cs_gpio != nullptr) {
    nand_cfg.cs_callback = set_cs;
    nand_cfg.cs_context = this;
  }

  err = spi_nand_flash_init_device(&nand_cfg, &_nand_device);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "spi_nand_flash_init_device failed: %s", esp_err_to_name(err));
    _nand_device = nullptr; // The NAND driver frees its device on init failure.
    spi_bus_remove_device(_spi_device);
    _spi_device = nullptr;
    return false;
  }

  // Mount the FATFS filesystem.
  esp_vfs_fat_mount_config_t mount_cfg = {};
  mount_cfg.format_if_mount_failed = _config.format_if_mount_failed;
  mount_cfg.max_files = _config.max_files;
  mount_cfg.allocation_unit_size = _config.allocation_unit_size;
  mount_cfg.disk_status_check_enable = false;
  mount_cfg.use_one_fat = false;

  err = esp_vfs_fat_nand_mount(_mount_path, _nand_device, &mount_cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_vfs_fat_nand_mount failed: %s", esp_err_to_name(err));
    spi_nand_flash_deinit_device(_nand_device);
    _nand_device = nullptr;
    spi_bus_remove_device(_spi_device);
    _spi_device = nullptr;
    return false;
  }

  _mounted = true;
  ESP_LOGI(TAG, "mounted at %s", _mount_path);
  return true;
}

bool SpiNandStorage::format() {
  if (!_mounted) {
    ESP_LOGE(TAG, "format: not mounted");
    return false;
  }

  // Step 1: Unmount the current filesystem.
  esp_err_t err = esp_vfs_fat_nand_unmount(_mount_path, _nand_device);
  _mounted = false;
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "format: unmount failed: %s", esp_err_to_name(err));
    return false;
  }

  // Step 2: Erase the entire NAND chip (may take several seconds).
  ESP_LOGI(TAG, "format: erasing chip...");
  err = spi_nand_erase_chip(_nand_device);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "format: erase_chip failed: %s", esp_err_to_name(err));
    return false;
  }

  // Step 3: Remount — the erased chip has no valid filesystem, so
  // format_if_mount_failed creates a fresh FATFS automatically.
  esp_vfs_fat_mount_config_t mount_cfg = {};
  mount_cfg.format_if_mount_failed = true;
  mount_cfg.max_files = _config.max_files;
  mount_cfg.allocation_unit_size = _config.allocation_unit_size;
  mount_cfg.disk_status_check_enable = false;
  mount_cfg.use_one_fat = false;

  err = esp_vfs_fat_nand_mount(_mount_path, _nand_device, &mount_cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "format: remount failed: %s", esp_err_to_name(err));
    return false;
  }

  _mounted = true;
  ESP_LOGI(TAG, "format: complete, remounted at %s", _mount_path);
  return true;
}

void SpiNandStorage::deinit() {
  if (_mounted) {
    const esp_err_t err = esp_vfs_fat_nand_unmount(_mount_path, _nand_device);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "esp_vfs_fat_nand_unmount failed: %s", esp_err_to_name(err));
    }
    _mounted = false;
  }

  if (_nand_device != nullptr) {
    spi_nand_flash_deinit_device(_nand_device);
    _nand_device = nullptr;
  }

  if (_spi_device != nullptr) {
    spi_bus_remove_device(_spi_device);
    _spi_device = nullptr;
  }
}

bool SpiNandStorage::is_mounted() const { return _mounted; }

const char *SpiNandStorage::mount_path() const { return _mount_path; }

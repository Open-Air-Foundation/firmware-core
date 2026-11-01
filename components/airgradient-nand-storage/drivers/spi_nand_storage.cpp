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
#include "freertos/FreeRTOS.h"

static constexpr const char *TAG = "SpiNandStorage";

namespace {
// Go has one NAND device. Registration happens before the filesystem mounts;
// deinit occurs only after filesystem users have stopped.
spi_device_handle_t software_cs_device = nullptr;
const gpio::Hal *software_cs_gpio = nullptr;
int software_cs_pin = gpio::INVALID_PIN;
} // namespace

extern "C" esp_err_t __real_spi_device_transmit(spi_device_handle_t, spi_transaction_t *);

extern "C" esp_err_t __wrap_spi_device_transmit(spi_device_handle_t device, spi_transaction_t *t) {
  if (device != software_cs_device || software_cs_gpio == nullptr) {
    return __real_spi_device_transmit(device, t);
  }
  // Lock the shared SPI bus before asserting NAND CS, so display traffic
  // cannot reach the NAND while the expander transaction is in progress.
  esp_err_t err = spi_device_acquire_bus(device, portMAX_DELAY);
  if (err != ESP_OK)
    return err;
  if (software_cs_gpio->set_level(software_cs_pin, 0)) {
    err = __real_spi_device_transmit(device, t);
  } else {
    err = ESP_FAIL;
  }
  // Never release the SPI bus with an unknown NAND select level. A reset is
  // preferable to allowing display traffic to corrupt flash commands.
  ESP_ERROR_CHECK(software_cs_gpio->set_level(software_cs_pin, 1) ? ESP_OK : ESP_FAIL);
  spi_device_release_bus(device);
  return err;
}

SpiNandStorage::SpiNandStorage(const Config &config) : _config(config) {
  snprintf(_mount_path, sizeof(_mount_path), "%s", config.mount_path ? config.mount_path : "/nand");
}

SpiNandStorage::~SpiNandStorage() { deinit(); }

bool SpiNandStorage::init() {
  if (_mounted) {
    return true;
  }

  if (_config.cs_pin == GPIO_NUM_MAX) {
    ESP_LOGE(TAG, "cs_pin not configured");
    return false;
  }

  if (_config.cs_gpio != nullptr &&
      (software_cs_device != nullptr || !_config.cs_gpio->set_level(_config.cs_pin, 1))) {
    ESP_LOGE(TAG, "software CS unavailable");
    return false;
  }

  // Register SPI device on the pre-initialised bus.
  spi_device_interface_config_t dev_cfg = {};
  dev_cfg.mode = 0;
  dev_cfg.clock_source = SPI_CLK_SRC_DEFAULT;
  dev_cfg.duty_cycle_pos = 128;
  dev_cfg.clock_speed_hz = _config.clock_speed_hz;
  dev_cfg.spics_io_num = _config.cs_gpio == nullptr ? static_cast<int>(_config.cs_pin) : -1;
  dev_cfg.flags = _config.spi_device_flags;
  dev_cfg.queue_size = 10;

  esp_err_t err = spi_bus_add_device(_config.spi_host, &dev_cfg, &_spi_device);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
    return false;
  }

  if (_config.cs_gpio != nullptr) {
    software_cs_gpio = _config.cs_gpio;
    software_cs_pin = _config.cs_pin;
    software_cs_device = _spi_device;
  }

  // Initialise the NAND flash device.
  spi_nand_flash_config_t nand_cfg = {};
  nand_cfg.device_handle = _spi_device;
  nand_cfg.gc_factor = 4;
  nand_cfg.io_mode = SPI_NAND_IO_MODE_SIO;
  nand_cfg.flags = _config.spi_device_flags;

  err = spi_nand_flash_init_device(&nand_cfg, &_nand_device);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "spi_nand_flash_init_device failed: %s", esp_err_to_name(err));
    // The vendor frees the failed handle without clearing the out parameter.
    _nand_device = nullptr;
    deinit();
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
    deinit();
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
    if (_spi_device == software_cs_device) {
      software_cs_device = nullptr;
      software_cs_gpio = nullptr;
      software_cs_pin = gpio::INVALID_PIN;
    }
    spi_bus_remove_device(_spi_device);
    _spi_device = nullptr;
  }
}

bool SpiNandStorage::is_mounted() const { return _mounted; }

const char *SpiNandStorage::mount_path() const { return _mount_path; }

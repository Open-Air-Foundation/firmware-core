/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "common.h"

#include <cstdio>
#include <cstdlib>

#ifndef TEST_HOST
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_rom_sys.h"
#include "esp_system.h"

namespace {
constexpr const char *TAG = "HeapDiagnostics";

void log_heap_allocation_failure(size_t size, uint32_t caps, const char *function_name) {
  // Avoid normal logging, formatting buffers, or stack scans on the failure path.
  const bool in_isr = xPortInIsrContext();
  const char *task = in_isr ? "ISR" : pcTaskGetName(nullptr);
  const size_t free = heap_caps_get_free_size(caps);
  const size_t largest = heap_caps_get_largest_free_block(caps);
  esp_rom_printf("[HEAP_FAIL] size=%u caps=0x%08x function=%s task=%s free=%u largest=%u\n",
                 static_cast<unsigned>(size), static_cast<unsigned>(caps),
                 function_name ? function_name : "unknown", task ? task : "unknown",
                 static_cast<unsigned>(free), static_cast<unsigned>(largest));
}
} // namespace
#endif

void enable_heap_failure_logging() {
#ifndef TEST_HOST
  const esp_err_t err = heap_caps_register_failed_alloc_callback(log_heap_allocation_failure);
  if (err != ESP_OK) {
    AG_LOGE(TAG, "Allocation-failure callback registration failed: %s", esp_err_to_name(err));
    return;
  }
  AG_LOGI(TAG, "Allocation-failure callback registered");
#endif
}

std::string build_serial_number() {
#ifndef TEST_HOST
  uint8_t mac_address[6];
  esp_err_t err = esp_read_mac(mac_address, ESP_MAC_WIFI_STA);
  if (err != ESP_OK) {
    printf("Failed to get MAC address (%s)\n", esp_err_to_name(err));
    return {};
  }

  char result[13] = {0};
  snprintf(result, sizeof(result), "%02x%02x%02x%02x%02x%02x", mac_address[0], mac_address[1],
           mac_address[2], mac_address[3], mac_address[4], mac_address[5]);
  return std::string(result);
#else
  return {};
#endif
}

std::string build_firmware_version() {
#ifndef TEST_HOST
  const esp_app_desc_t *desc = esp_app_get_description();
  if (desc == nullptr || desc->version[0] == '\0') {
    return {};
  }

  return std::string(desc->version);
#else
  return {};
#endif
}

uint32_t generate_random_number(uint8_t length) {
  static constexpr uint8_t MAX_SUPPORTED_DIGITS = 9;

  if (length == 0 || length > MAX_SUPPORTED_DIGITS) {
    return 0;
  }

  uint32_t base = 1;
  for (uint8_t i = 1; i < length; ++i) {
    base *= 10;
  }

  const uint32_t min_value = (length == 1) ? 0 : base;
  const uint32_t span = (length == 1) ? 10 : (base * 9);

#ifndef TEST_HOST
  const uint32_t random_value = esp_random();
#else
  const uint32_t random_value = static_cast<uint32_t>(std::rand());
#endif

  return (random_value % span) + min_value;
}

void reboot() {
#ifndef TEST_HOST
  esp_restart();
#endif
}

void format_ipv4_be(uint32_t ip_be, char out[16]) {
  std::snprintf(out, 16, "%u.%u.%u.%u", static_cast<unsigned>(ip_be & 0xFF),
                static_cast<unsigned>((ip_be >> 8) & 0xFF),
                static_cast<unsigned>((ip_be >> 16) & 0xFF),
                static_cast<unsigned>((ip_be >> 24) & 0xFF));
}

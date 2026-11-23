/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef COMMON_H
#define COMMON_H

#include "ag_log.h"

#include <cstdint>
#include <string>

#ifndef TEST_HOST
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

// Builds a 12-character hex serial number from the Wi-Fi STA MAC address.
// Returns an empty string on failure or when running under TEST_HOST.
std::string build_serial_number();

// Reads the firmware version from the running app description.
// Returns an empty string on failure or when running under TEST_HOST.
std::string build_firmware_version();

// Generates a random decimal number with exactly the requested digit length.
// Returns 0 when the requested length is invalid for uint32_t output.
uint32_t generate_random_number(uint8_t length);

// Reboots the MCU. No-op when running under TEST_HOST.
void reboot();

// Formats a network-byte-order IPv4 address into a dotted-decimal string
// (e.g. 0x0104a8c0 -> "192.168.4.1"). The output buffer must be at least
// 16 bytes (covers "255.255.255.255" + NUL). The caller is responsible
// for ensuring the buffer is large enough.
void format_ipv4_be(uint32_t ip_be, char out[16]);

// Task-context diagnostic. All sizes are bytes; min values are historical.
// stack_min is the calling task's minimum untouched stack, not current usage.
inline void log_heap(const char *tag, const char *label) {
#ifndef TEST_HOST
  constexpr uint32_t INTERNAL_CAPS = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  AG_LOGI(tag,
          "[HEAP] %s free=%u min=%u largest=%u internal=%u internal_largest=%u "
          "task=%s stack_min=%u",
          label, static_cast<unsigned>(esp_get_free_heap_size()),
          static_cast<unsigned>(esp_get_minimum_free_heap_size()),
          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT)),
          static_cast<unsigned>(heap_caps_get_free_size(INTERNAL_CAPS)),
          static_cast<unsigned>(heap_caps_get_largest_free_block(INTERNAL_CAPS)),
          pcTaskGetName(nullptr), static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
#else
  (void)tag;
  (void)label;
#endif
}

#endif // COMMON_H

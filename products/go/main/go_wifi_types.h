/**
 * AirGradient Go — WifiService public types
 *
 * Standalone event-payload header shared between go_events.h and
 * WifiService. Kept separate so the event header can stay free of the
 * shared-component includes WifiService needs internally.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef GO_WIFI_TYPES_H
#define GO_WIFI_TYPES_H

#include <cstdint>

#include "types/wifi_types.h"

// Provisioning event payload posted onto the orchestrator queue. Carries
// `disable_cloud` and `static_ip` inline so the orchestrator can persist
// metadata without querying live service state after the event.
// `static_ip` is zeroed when the user selected DHCP.
struct ProvisioningEventPayload {
  uint8_t event;       // ProvisioningEvent enum value
  uint8_t transport;   // ProvisioningTransport enum value
  uint8_t stop_reason; // ProvisioningStopReason enum value
  uint32_t ip;
  bool disable_cloud;
  bool portal_used;
  WifiStaticIpConfig static_ip;
};

#endif // GO_WIFI_TYPES_H

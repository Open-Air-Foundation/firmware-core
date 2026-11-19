# Provisioning Captive Portal

Discovery, portal behavior, and transport implementation constraints. See
[lifecycle](lifecycle.md) for startup, route ownership, mDNS replacement, and
teardown order; start at the [component README](../README.md) for integration.

## Captive Portal Detection

The portal registers OS-specific probe URLs (`/hotspot-detect.html`,
`/generate_204`, `/connecttest.txt`, `/canonical.html`, etc.) that
return `302 Found` with `Location: http://192.168.4.1/`. This triggers
the in-app captive browser on iOS, Android, Windows, and Firefox.

The `Location` header **must** be an absolute URL — iOS CNA silently
follows relative redirects without popping the captive browser.

## Implementation Notes

- The provisioning mDNS profile is manual and contains one
  `_http._tcp` service on `http_port` with no TXT records. It is
  installed only after the HTTP listener starts and is stopped and
  cleared before captive DNS, route, or listener teardown.
- The captive-portal probe redirect uses a hardcoded
  `http://192.168.4.1/` in the `Location` header. This is the lwIP
  soft-AP default gateway IP. If the AP subnet is ever reconfigured,
  this must be updated in
  `internal/wifi_portal_transport.cpp :: handle_captive_probe()`.
- The captive portal page (`assets/portal.html`) does not auto-trigger
  a scan on load — the network list stays hidden until the user
  presses the "Scan for Wi-Fi networks" button (which then becomes
  "Rescan" after the first scan completes). This avoids paying the
  multi-second scan dwell on every page reload during the user's
  onboarding flow.

## Verification

[Component tests](../README.md#tests) cover portal handlers, DNS codecs, and
mDNS lifecycle. Captive browser behavior and live DNS sockets require hardware
checks. The portal is implemented in
[wifi_portal_transport.cpp](../internal/wifi_portal_transport.cpp) with its
[embedded page](../assets/portal.html).

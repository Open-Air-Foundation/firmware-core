# AirGradient Go Feature Overview

This overview explains current Go behavior for product, support, and other
non-engineering team members, without requiring firmware or hardware
implementation knowledge.

AirGradient Go is a portable air quality monitor for measuring personal air
quality while moving or while placed in one location. It combines environmental
sensors, GPS route tracking, an e-paper display, battery operation, BLE app
connectivity, Wi-Fi support, local storage, LEDs, and buzzer feedback.

## Product Summary

AirGradient Go is designed for people who want to understand the air quality
around them throughout the day. It can be carried outdoors, used during travel,
or placed in a fixed location. The device can work with a phone app over BLE,
connect to Wi-Fi in stationary use, or sleep between measurements in Energy
Saving mode with brief BLE broadcasts.

Key product ideas:

- Portable air quality monitoring with built-in battery.
- Live on-device readings on a low-power e-paper display.
- GPS-based route tracking for mobile measurements.
- Secure BLE connection to a phone for live data, settings, and history export.
- Wi-Fi connectivity, LAN discovery, and local integration for stationary use.
- Local storage for route and measurement history.
- Visual and audio feedback through LEDs and buzzer, where hardware supports it.

## What It Measures

AirGradient Go measures the main air quality and environmental values expected
from a portable monitor.

| Category | Measurement | User-Facing Meaning |
|---|---|---|
| Particulate Matter | PM1.0, PM2.5, PM10 | Fine particles in the air, including smoke, dust, and pollution |
| CO2 | Carbon dioxide in ppm | Ventilation and indoor air freshness indicator |
| Gases | TVOC index, NOx index | General indicators for volatile organic compounds and nitrogen oxides |
| Climate | Temperature, humidity | Ambient comfort and sensor context |
| Pressure | Barometric pressure, altitude estimate | Environmental context and altitude support |
| Location | GPS latitude, longitude, altitude, fix quality, satellites | Route tracking and map-based context |
| Power | Battery percentage, voltage, charging state | Device power and charging status |

If a sensor is warming up or temporarily unavailable, the device does not treat
that value as zero. The device display uses unavailable indicators, such as
dashes.

## Operating Modes

AirGradient Go has three main operating modes. The mode controls which radios
and connectivity features are active.

| Mode | Main Use | Connectivity | Typical Behavior |
|---|---|---|---|
| Portable | Carrying the device with a phone | BLE | Streams live readings to the app and supports route history export |
| Stationary | Leaving the device in one place | Wi-Fi | Supports local-network integrations and optional AirGradient cloud services |
| Energy Saving | Long battery operation | Brief BLE measurement broadcasts; no connectable BLE or Wi-Fi | Measures locally with GPS asleep and tracking unavailable |

The default mode on a fresh device is Portable.

## Tracking and Route Logging

Tracking is one of the key Go features. While recording, the device
combines air quality readings with GPS information and stores them as a route.

Tracking supports:

- Starting, pausing, resuming, and stopping from the device menu.
- The same controls through a connected phone over BLE.
- GPS plus sensor data stored together as route points.
- A session ID identifying each stored tracking session.
- Exporting stored route history to the phone over BLE.

Pause stops new sensor/GPS route points while retaining the session. Resume
continues the same route without filling in the paused interval. Live readings,
charts, GPS, and existing data export continue. The display replaces the recording
dot with a pause icon. A paused route must be stopped before it can be deleted.

Tracking is available only in Portable. Entering Stationary or Energy Saving ends
Recording or Paused sessions and preserves saved routes. Returning to Portable
leaves tracking idle. Starting a route can wait for GPS to wake while sensor
points are already recorded; Always Off keeps GPS asleep. Entering or leaving
Energy Saving requests a system restart. If power stays on, the mode change
continues without restarting.

The device also keeps a short temporary measurement history for on-device charts.
This chart data is separate from the persistent route log.

## Display and Navigation

The Go uses a 128 x 250 pixel monochrome e-paper display. E-paper is useful for a
portable device because it remains visible without constant display power.

The display can show:

- Current air quality readings.
- Battery and charging status.
- GPS, BLE, Wi-Fi, and tracking status icons.
- Menu screens and settings.
- Pairing passkeys.
- Setup and provisioning screens.
- Warning and shutdown messages.
- Small charts based on recent measurements.

User navigation is handled through three touch areas and physical buttons.
Common actions include opening the menu, changing settings, starting or stopping
tracking, confirming actions, and locking or unlocking the interface.

**Pinned Chart** keeps a chosen measurement chart on Home, including while
locked. It starts as PM2.5; None disables it. Unlocked Up/Down browsing shows
each available metric with its value, units, Min/Max, and history without
changing the pinned choice. It is the first setting in Display & Touch and is
also configurable through BLE and the Local Server.

Below a five-minute measurement interval, enclosure heating makes temperature
and humidity unsuitable for ambient display, so their cells and charts are
hidden. They remain in stored measurements and cloud, BLE, and local-server
payloads. A pinned temperature or humidity chart resumes when the interval
returns to five minutes or longer. Below five minutes, the empty chart area
shows “Pin a chart here” for None or “Not available” for a hidden thermal chart.

### Shake-to-Refresh

On V1, while Go is awake, shake it side to side using a natural forearm swing, roughly
three left/right pairs in about one second. An accepted shake gives a short
beep when the buzzer is enabled and requests fresh readings. If a regular
measurement is already requested, its result satisfies the refresh.

The display shows persistent `Waiting...`, `Preparing...`, or `Measuring...`
progress until the result arrives. Progress remains visible when opening menus,
and ordinary snackbar messages cannot replace it. Further shakes while refresh
is pending do not queue more measurements or beep again.

Manual refresh updates current readings without adding chart-history samples
or route points, changing the regular measurement timer, or triggering an extra
cloud upload. PM warmup can delay the result. Shake-to-wake from deep sleep is
not implemented. V2 has no shake feature; its accelerometer is used only by
the Accelerometer Test.

## Lock and Power Button Behavior

The device has a lock state so accidental touches do not change settings while
it is being carried.

- A short press on the power button locks or unlocks the interface, except
  during startup and setup screens.
- Touch navigation is ignored while locked.
- A long press powers the device off on battery or enters charging-only
  operation with USB attached.
- If the user keeps holding the power button after shutdown on battery, the
  hardware can restart the device.
- Auto-lock can be configured or disabled.

## Phone App and BLE Features

In Portable mode, the Go acts as a BLE peripheral for the phone app.

Main BLE features:

- Device discovery as `AirGradient Go` with a short serial suffix.
- Secure pairing using a six-digit passkey shown on the e-paper display.
- Bonding, so future connections do not need the passkey again.
- Live sensor readings.
- GPS data while tracking or when GPS is enabled.
- Battery, charging, tracking, GPS, and storage status.
- Reading and changing device settings.
- Starting, pausing, resuming, and stopping tracking.
- Clearing data.
- Factory reset.
- CO2 calibration command.
- GPS aiding from the phone to help GPS start faster.
- Downloading stored route history.
- BLE firmware update in Portable mode.

Only one phone can be connected at a time. When a phone is connected, the device
stops advertising until that phone disconnects.

## Wi-Fi, Local Server, and Cloud Features

In Stationary mode, the Go uses Wi-Fi instead of the Portable BLE data stream.
This mode is intended for use when the device is placed in one location.

Wi-Fi and cloud features include:

- Connecting with saved Wi-Fi credentials.
- Wi-Fi setup through BLE or a captive portal flow.
- Dedicated factory Wi-Fi in manufacturing mode.
- Saved network reconnect behavior after temporary outages.
- Optional cloud posting of latest readings.
- Static IP support when configured during provisioning.
- Wi-Fi firmware update checks in Stationary mode while cloud connection is
  enabled.
- Local discovery through `_airgradient._tcp` mDNS, with a hostname based on the
  device serial number.
- A versioned local HTTP API for corrected live measurements, system identity,
  and selected active configuration.

Normal setup uses saved networks or asks for credentials; it does not fall back
to factory Wi-Fi automatically.

The device can also receive Wi-Fi credentials over its bonded Portable BLE
link. A request turns Wi-Fi on for scanning or verification; a successful
connection or the inactivity deadline turns it off again. Go stays in Portable
and uses successfully saved credentials when it later enters Stationary.

### Local Network Integration

**Settings → Operations → Local Server** enables or disables local access. It
is shown only in Stationary and defaults to Enable. Disabling it stops the API
and discovery, enables Wi-Fi power saving, and keeps cloud connectivity
available. The selection is saved across restarts and mode changes.

When enabled and connected, LAN clients can query the local API by address
once its routes are ready. After mDNS activation also succeeds,
clients can discover the Go as `airgradient_<serial>.local`. The API exposes the
latest corrected common air measurements, battery percentage, battery and
charging-input voltages, identity, and uptime. GPS position and route history
are not part of this local v1 surface.

Local clients can change this selected configuration:

- PM display standard: mass concentration or US AQI.
- Temperature unit: Celsius or Fahrenheit.
- Altitude unit: meters or feet for the device display.
- Pinned Home chart.
- Measurement interval: any whole second from 1 through 3600.
- GPS mode, LED indicator and brightness, touch LED intensity on V1, and buzzer.
- Cloud connection enabled or disabled.
- Configuration source: cloud, local, or both.
- PM2.5, temperature, and humidity measurement corrections.
- CO2 automatic background calibration period (1 to 200 days), or disabled.
- TVOC and NOx learning-time offsets.

Remote setting changes can take a moment to appear. Local integrations can also
request CO2 calibration, an LED diagnostic, or the live GPS Test screen.
Accepting a request does not confirm that a diagnostic ran or that calibration
succeeded.

The local API uses plain HTTP without API authentication or TLS. It is designed
for a trusted private LAN and should not be exposed directly to an untrusted or
public network.

### Cloud and Configuration Controls

Two settings control different concerns:

| Setting | Effect |
|---|---|
| Cloud connection | Enables cloud uploads, remote settings fetches, and automatic Stationary update checks. Turning it off leaves enabled local access available; work already in progress can finish. |
| Configuration source | Selects cloud, local-network clients, or both for remote settings changes. Phone BLE controls and on-device settings remain available. |

Cloud configuration applies supported display, measurement-interval, GPS, LED,
buzzer, sensor-learning, CO2 calibration-period, and measurement-correction
settings. It cannot change the
cloud-connection or configuration-source controls themselves. When the source
is `local`, the device does not issue cloud config Fetch requests, although
cloud measurement upload can continue. When the source is `cloud`, normal local
setting changes are blocked. A local client can still change the source itself
to local or both, allowing local control to be restored.

## Energy Saving Operation

Energy Saving is the UI name for Offline mode. It sleeps between measurements
and briefly broadcasts readings over BLE during timer wakes.

In Energy Saving:

- The device continues measuring locally.
- Tracking and GPS Test are unavailable; GPS receives a long sleep request.
- No connectable BLE data stream is active; nearby scanners can receive the
  brief public measurement broadcasts without pairing.
- No Wi-Fi cloud posting is active.
- Firmware update is not available until the user changes to Portable or
  Stationary mode.

## Battery and Charging Features

AirGradient Go is battery-powered and includes battery management features for
portable use.

User-visible battery features:

- Battery percentage display.
- Charging status display.
- Charging while powered off, with Charging and Battery Full screens. Hold
  the power button to turn on, or unplug to power off.
- USB plugged-in indication.
- A low-battery shutdown screen when sustained low cell voltage triggers
  protective shutdown.
- Battery-temperature protection, including charge inhibition, invalid-NTC
  handling, and distinct hot/cold protective shutdowns.
- Charger-controlled charge completion and recharge; a displayed 100% does not
  itself stop charging.

The firmware also manages sensor power to reduce drain. For example, the PM
sensor's fan can sleep between measurements when the measurement interval is
long enough.

## LEDs and Buzzer Feedback

AirGradient Go supports visual feedback through LEDs. On supported board
variants, it also supports a buzzer.

LED features:

- LED Indicator selects AQI (from PM2.5) or CO2; default AQI.
- Independent LED Brightness and touch feedback settings.
- Invalid readings turn the selected indicator off.
- Boot and other short animations.

Buzzer and melody features:

- Buzzer on/off setting.
- Boot sound pattern.
- First-time welcome chime.
- Short acknowledgment beep for an accepted shake refresh.
- Play Melody under Settings > Hardware Test.
- Built-in melody choices such as Chime and Tetris.
- Synchronized buzzer and LED effects for selected melodies.

## First-Time Setup

On a fresh device, the Go shows Getting Started after initialization while
sensors warm up. Holding Menu opens Home immediately; readings show dashes
until the first measurement arrives.
This flow includes a QR code labeled `Scan for guide` that points users to the
Go usage guide. The device can be used without following the guide.

The Getting Started screen is shown until the user completes a first meaningful
engagement, such as:

- holding Menu on `Hold menu to start`,
- successfully pairing with the phone app, or
- changing the operating mode.

Factory reset clears this state, so the setup guide can be shown again on a
reset or refurbished unit.

## Configurable Settings

The main menu offers Exit Menu, Start Tracking / Tracking, Operating Mode, and
Settings. Stationary and Energy Saving hide the tracking row. During a session, Tracking opens
Exit, Back, Status, Pause Tracking / Resume Tracking, and Stop Tracking. Operating Mode lists Portable, Stationary, and
Energy Saving. Settings contains Operations, Display & Touch, Hardware Test, Clear
Data, Setup Guide, and About Device.

Operations groups measurement interval, CO2 calibration, GPS mode, and buzzer.
Display & Touch starts with Pinned Chart, followed by temperature/altitude units,
PM display, auto-lock, LED Indicator, LED Brightness, and Touch LED (V1 only).
Stationary adds Local Server under Operations.

Submenus keep Exit and Back; opening Tracking selects Status to show session
details. Settings and its list submenus select their first content item, while
value choices select the saved value. Back restores the row that opened the
child page.

Tracking Status shows session state, recording time excluding pauses, elapsed
time including pauses, point count, last-point age, and GPS time. A second view
shows GPS signal and location details. Values refresh every five seconds while
the page is open. Unavailable values show `--`.

The page is available only in Portable and does not auto-lock; use Power to
lock or leave to restore normal auto-lock.

The Go stores user settings on the device, so they survive power cycles.

Common configurable settings include:

- Measurement interval choices of 3s, 10s, 30s, 60s, 5m, 15m, and 1h. A
  different valid value supplied through a firmware interface appears as an
  exact custom interval until the user selects a fixed choice. Energy Saving
  has its own saved interval, with menu choices of 5m, 15m, and 1h.
- Temperature unit: Celsius or Fahrenheit.
- Altitude unit: meters or feet.
- PM display: micrograms per cubic meter or US AQI.
- GPS mode: always off, on when tracking, or always on.
- Operating mode: Portable, Stationary, or Energy Saving.
- Auto-lock timeout.
- LED Indicator (AQI or CO2) and LED Brightness.
- Touch LED brightness (V1 only).
- Buzzer enabled or disabled.
- Static IP and cloud preference for Stationary mode.

Action-style settings are also available from the device or app, including CO2
calibration, clearing stored data, and factory reset.

## Firmware Updates

Firmware update behavior depends on the current operating mode.

| Mode | Firmware Update Path |
|---|---|
| Portable | Phone-initiated update over BLE |
| Stationary | Device-initiated update check over Wi-Fi while cloud connection is enabled |
| Energy Saving | No update path until the operating mode changes |

During a firmware update, normal sensing and data transfer are paused so the
update can complete safely. During a committed Stationary update, an
already-active local endpoint remains read-only while STA stays connected.
Measures and config GET requests return their last cached snapshots, while
config writes and actions are rejected until a non-rebooting update attempt
finishes. An endpoint that was already disabled remains disabled.

## Data Storage and Export

The Go has two kinds of measurement storage:

- Temporary chart history for recent readings shown on the device.
- Persistent route history for tracking sessions.

Saved route history survives deep sleep and power cycles. Abrupt power loss
can lose recent points that have not yet been saved.
Users can export routes through the phone app over BLE or clear stored data
from the device menu or app. Clearing data also ends any active tracking session.

## Product Notes for Team Members

- The firmware supports Go V1 and V2, selected by the eFuse board ID.
- V2 has four back LEDs and no touch LEDs. It hides the Touch LED setting and
  skips that Peripheral Test step and result. BLE and local-server touch LED
  writes are rejected; cloud updates ignore the field.
- V2 keeps the Accelerometer Test but has no shake-to-refresh.
- CO2 sensor hardware can vary by build; the product behavior is still presented
  to users as CO2 measurement.
- Temperature and humidity may come from a dedicated sensor or from another
  sensor fallback depending on hardware availability.
- Missing readings should be described as unavailable, warming up, or not ready,
  not as zero.
- Portable BLE is the main app experience for carrying the device.
- Stationary Wi-Fi is the main experience for fixed-location use.
- Energy Saving has no Wi-Fi or connectable BLE, but briefly broadcasts readings
  over BLE during timer wakes.

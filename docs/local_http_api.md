# Local HTTP API

AirGradient devices can expose a versioned HTTP API on a local network. The
API lets local clients read the latest measurements, inspect active device
configuration, submit supported configuration changes, and trigger supported
actions. Product and operating-mode support varies; clients must discover a
device's capabilities rather than assuming every endpoint, field, or action is
available.

## Availability and Discovery

A device exposes the API only while its product and active operating mode allow
local network access. When it does, it advertises the following mDNS service:

| Property | Value |
|---|---|
| Service type | `_airgradient._tcp` |
| Hostname | `airgradient_<serial>.local` |
| API version TXT record | `api=1` |
| Identity TXT records | `vendor=AirGradient`, `model`, `serialno`, `fw_ver` |

`serialno` is the same value returned as `serialNumber` by the measures
endpoint. Clients should use the service record's advertised host and port;
they must not assume a fixed port. If mDNS is unavailable on the client
network, use the device's LAN address instead.

## Security

The local API uses plain HTTP. It does not provide TLS, authentication,
authorization, or a CORS policy. Use it only on a trusted, appropriately
isolated LAN and never expose it directly to an untrusted or public network.

## Quick Start

Replace the hostname with the address discovered through mDNS or the device's
LAN address.

```sh
export AG_URL="http://airgradient_ABCDEF123456.local"
curl "$AG_URL/api/v1/measures"
curl "$AG_URL/api/v1/config"
```

## Endpoints

| Method | Path | Success | Purpose |
|---|---|---:|---|
| `GET` | `/api/v1/measures` | `200` | Read the latest valid measurements and device identity. |
| `GET` | `/api/v1/config` | `200` | Read the active configuration supported by the product. |
| `PUT` | `/api/v1/config` | `202` | Submit a partial configuration update. |
| `POST` | `/api/v1/actions/calibrate-co2` | `200` | Request CO2 calibration when supported. |
| `POST` | `/api/v1/actions/test-leds` | `200` | Request an LED diagnostic when supported. |
| `POST` | `/api/v1/actions/test-gps` | `200` | Open the live GPS test when supported. |

An endpoint can be absent when the product does not expose that capability. In
that case, the HTTP server returns its normal `404` response. A registered
action that the current product does not support returns a structured `404`
error instead.

### Read Measurements

`GET /api/v1/measures` returns a JSON object containing device identity and
the latest valid readings. `serialNumber`, `model`, `firmware`, and `boot` are
always present. `boot` is the completed uptime in minutes. `wifiRssi` is in
dBm and is present only when link quality is available.

All other measurement fields are optional. A product omits a field when it
lacks the sensor or the latest value is invalid; it does not send JSON `null`.

#### Identity and Link Fields

| Field | JSON Type | Units / Values | Description |
|---|---|---|---|
| `serialNumber` | String | — | Device serial number. Always present. |
| `model` | String | — | AirGradient product model identifier. Always present. |
| `firmware` | String | — | Firmware version. Always present. |
| `boot` | Integer | Completed minutes | Uptime for the current boot session, saturated at `4294967295`. Always present. |
| `wifiRssi` | Integer | dBm | Received Wi-Fi signal strength. Omitted when unavailable. |

#### Air Measurements

| Field | JSON Type | Units / Values | Description |
|---|---|---|---|
| `co2` | Integer | ppm | Carbon dioxide concentration. |
| `pm01` | Number | µg/m³ | PM1.0 mass concentration. |
| `pm25` | Number | µg/m³ | PM2.5 mass concentration. |
| `pm10` | Number | µg/m³ | PM10 mass concentration. |
| `temperature` | Number | °C | Air temperature. This remains Celsius regardless of a display-unit setting. |
| `humidity` | Number | %RH | Relative humidity. |
| `tvocIndex` | Integer | Index | TVOC gas index. |
| `tvocRaw` | Integer | Sensor raw value | TVOC sensor raw reading. |
| `noxIndex` | Integer | Index | NOx gas index. |
| `noxRaw` | Integer | Sensor raw value | NOx sensor raw reading. |

PM mass is rounded to one decimal place. Temperature and humidity are rounded
to two decimal places.

#### Particle Count Fields

| Field | JSON Type | Units / Values | Description |
|---|---|---|---|
| `pm003Count` | Integer | Particle count | Particles with a 0.3 µm size threshold. |
| `pm005Count` | Integer | Particle count | Particles with a 0.5 µm size threshold. |
| `pm01Count` | Integer | Particle count | Particles with a 1.0 µm size threshold. |
| `pm02Count` | Integer | Particle count | Particles with a 2.5 µm size threshold. |
| `pm50Count` | Integer | Particle count | Particles with a 5.0 µm size threshold. |
| `pm10Count` | Integer | Particle count | Particles with a 10 µm size threshold. |

Particle counts are rounded to whole numbers. The sensor determines the count
sample volume; clients must not infer a concentration unit from these fields.

#### Power Fields

| Field | JSON Type | Units / Values | Description |
|---|---|---|---|
| `battPercent` | Integer | % | Battery charge percentage. |
| `battVolt` | Number | V | Battery voltage. |
| `chargeVolt` | Number | V | Charging-input voltage; it is not a charging-state indicator. |

Voltages are rounded to two decimal places.

```json
{
  "serialNumber": "ABCDEF123456",
  "model": "P-1PSG",
  "firmware": "1.2.3",
  "boot": 42,
  "wifiRssi": -58,
  "co2": 612,
  "pm25": 4.3,
  "temperature": 21.75,
  "humidity": 47.2
}
```

### Read Configuration

`GET /api/v1/config` returns the complete active subset supported by the
device. Products omit unsupported fields. A client should use this response as
the capability source for subsequent configuration writes.

The configuration catalog below defines all standardized v1 fields. The generic
parser enforces each JSON type and the listed enum values. Products define the
supported subset and enforce product-specific ranges, URLs, country formats,
and cross-field policy.

#### Device and Display Fields

| Field | JSON Type | Values | Description |
|---|---|---|---|
| `country` | String | Product-defined | Country or region identifier. |
| `pmStandard` | String | `ugm3`, `us-aqi` | PM display standard: mass concentration or US AQI. |
| `temperatureUnit` | String | `c`, `f` | Display temperature unit. Measurements remain Celsius. |
| `altitudeUnit` | String | `m`, `ft` | Display altitude unit. Raw GPS and stored route altitude remain in meters. |
| `measurementInterval` | Integer | Seconds; product-defined range | Measurement interval. |
| `gpsMode` | String | `off`, `tracking`, `always` | GPS operating mode. |
| `frontLedBrightness` | Integer | Product-defined range | Front LED brightness. |
| `backLedBrightness` | Integer | Product-defined range | Back or AQI LED brightness. |
| `touchLedIntensity` | Integer | Product-defined range | Touch LED intensity. |
| `buzzerEnabled` | Boolean | `true`, `false` | Buzzer enablement. |
| `ledMode` | String | `co2`, `pm`, `iaqs`, `off` | LED display mode. |
| `ledBarBrightness` | Integer | Product-defined range | LED bar brightness. |
| `displayBrightness` | Integer | Product-defined range | Display brightness. |

#### Connectivity and Sensor Fields

| Field | JSON Type | Values | Description |
|---|---|---|---|
| `postDataToCloud` | Boolean | `true`, `false` | Cloud measurement-posting enablement. |
| `cloudConnection` | Boolean | `true`, `false` | Cloud connection enablement. |
| `configurationControl` | String | `cloud`, `local`, `both` | Permitted remote configuration sources. |
| `co2AbcDays` | Integer | Days; product-defined range | CO2 automatic background-calibration period. |
| `tvocLearningOffset` | Integer | Hours; product-defined range | TVOC gas-index learning offset. |
| `noxLearningOffset` | Integer | Hours; product-defined range | NOx gas-index learning offset. |
| `mqttBrokerUrl` | String | Product-defined URL | MQTT broker URL. |
| `httpDomain` | String | Product-defined host or URL | HTTP service domain. |
| `corrections` | Object | [Correction Object](#correction-object) | PM2.5, temperature, and humidity corrections. |

#### Correction Object

`corrections` contains zero or more of `pm25`, `temperature`, and `humidity`.
Each is a correction entry. The table describes the entry shape used in both
GET responses and PUT requests.

| Field | JSON Type | Applies To | Description |
|---|---|---|---|
| `correctionAlgorithm` | String | All entries | Product-supported correction algorithm. |
| `slr` | Object or `null` | All entries | Simple linear regression parameters, or `null` when none apply. |
| `slr.intercept` | Number | All non-null SLRs | SLR intercept. |
| `slr.scalingFactor` | Number | All non-null SLRs | SLR scaling factor. |
| `slr.useEpa2021` | Boolean | `pm25` only | Whether the PM2.5 correction uses the EPA 2021 adjustment. |

Clients should send both `intercept` and `scalingFactor` for a non-null `slr`.
Products validate accepted algorithms, coefficients, and partial-update
semantics. `useEpa2021` is not valid for temperature or humidity corrections.

### Write Configuration

`PUT /api/v1/config` accepts a JSON object containing only the fields to
change. The body must be complete, valid JSON, and use only recognized fields.
For example, a client may request a supported PM display standard:

```sh
curl -X PUT "$AG_URL/api/v1/config" \
  -H "Content-Type: application/json" \
  --data '{"pmStandard":"us-aqi"}'
```

`202 Accepted` has an empty body. It confirms that the device accepted the
validated update for later processing; it does not confirm persistence or
runtime application. Poll `GET /api/v1/config` until the requested values are
visible, using a client-selected deadline. An empty object is a valid no-op
when writes are allowed.

### Trigger Actions

Actions are fire-and-forget requests with an empty `200` response. A successful
response confirms only that the device accepted the request; it does not report
start, progress, or completion. For example:

```sh
curl -X POST "$AG_URL/api/v1/actions/calibrate-co2"
```

The v1 action catalog contains these actions:

| Action | Endpoint | Request Body | Description |
|---|---|---|---|
| CO2 calibration | `POST /api/v1/actions/calibrate-co2` | None | Request CO2 calibration. |
| LED test | `POST /api/v1/actions/test-leds` | None | Request the device LED diagnostic. |
| GPS test | `POST /api/v1/actions/test-gps` | None | Open the device live GPS test. |

An action may be unavailable for a product, current device state, or device
policy. Unsupported actions return `404 not_found`; rejected actions return
`403 forbidden`; and temporarily unavailable actions return `503 busy`.

## Errors

Local API errors use an `application/json` response body. `field` is present
when a specific configuration field caused the error.

```json
{
  "error": {
    "code": "invalid_value",
    "field": "pmStandard",
    "message": "invalid value"
  }
}
```

| Status | Code | Client Action |
|---:|---|---|
| `400` | `invalid_body` | Correct the JSON request body. |
| `400` | `unknown_field` | Remove the unknown field or use a supported API version. |
| `400` | `invalid_value` | Correct the value or its type. |
| `403` | `forbidden` | The current policy or state does not allow the operation. |
| `404` | `not_found` | The requested known field or action is unsupported by this product. |
| `503` | `busy` | Retry later with client-controlled backoff. |
| `500` | `internal` | Treat as a device-side failure and retry only when appropriate. |

`503` responses do not include a `Retry-After` header. Paths outside the local
API route catalog use the HTTP server's normal `404` response rather than this
error envelope.

## Compatibility and Capabilities

The API version is in the path: `/api/v1/`. Within a version, clients must
preserve unknown response fields and tolerate omitted optional fields. A product
may support a subset of the shared measurement, configuration, and action
catalog. Read configuration before writing it, and handle `404 not_found` for
model-specific fields and actions.

## AirGradient Go Support

AirGradient Go exposes the local API in Stationary mode after Wi-Fi obtains an
IP address. It uses the common mDNS discovery contract above. The API remains
available by LAN address through a transient reconnect after it has first been
activated; its mDNS advertisement follows Wi-Fi address availability.

### Supported Endpoints

| Method | Path | Availability | Purpose |
|---|---|---|---|
| `GET` | `/api/v1/measures` | Always while the Go local API is active | Read the latest corrected measurements and device identity. |
| `GET` | `/api/v1/config` | Always while the Go local API is active | Read Go's active local-API configuration. |
| `PUT` | `/api/v1/config` | When local configuration writes are allowed | Submit supported Go configuration changes. |
| `POST` | `/api/v1/actions/calibrate-co2` | When actions are allowed | Request CO2 calibration. |
| `POST` | `/api/v1/actions/test-leds` | When actions are allowed | Request the LED diagnostic. |
| `POST` | `/api/v1/actions/test-gps` | When actions are allowed | Open the live GPS test. |

### Measures Fields

Go returns the following fields from `GET /api/v1/measures`. Identity fields
are always present. All other fields are present only when their latest value is
valid; `wifiRssi` is present only while Stationary Wi-Fi is online.

| Field Group | Fields |
|---|---|
| Identity | `serialNumber`, `model`, `firmware`, `boot` |
| Link | `wifiRssi` |
| Air measurements | `co2`, `pm01`, `pm25`, `pm10`, `temperature`, `humidity`, `tvocIndex`, `tvocRaw`, `noxIndex`, `noxRaw` |
| Particle counts | `pm003Count`, `pm005Count`, `pm01Count`, `pm02Count`, `pm50Count`, `pm10Count` |
| Power | `battPercent`, `battVolt`, `chargeVolt` |

Local measurement values use Go's corrected PM2.5, temperature, and humidity
values.

### Configuration Fields

Go returns these fields from `GET /api/v1/config` and accepts them in partial
`PUT /api/v1/config` requests when its local-write policy allows the request.

| Field Group | Supported Fields |
|---|---|
| Device behavior | `pmStandard`, `temperatureUnit`, `altitudeUnit`, `measurementInterval`, `gpsMode` |
| LEDs and buzzer | `frontLedBrightness`, `backLedBrightness`, `touchLedIntensity`, `buzzerEnabled` |
| Connectivity policy | `cloudConnection`, `configurationControl` |
| Sensor configuration | `co2AbcDays`, `tvocLearningOffset`, `noxLearningOffset`, `corrections` |

`altitudeUnit` changes only Go's display presentation. GPS and derived altitude
remain meters in raw and stored data.

### Actions

| Action | Endpoint | Availability |
|---|---|---|
| CO2 calibration | `POST /api/v1/actions/calibrate-co2` | Available when actions are allowed. |
| LED test | `POST /api/v1/actions/test-leds` | Available when actions are allowed. |
| GPS test | `POST /api/v1/actions/test-gps` | Available when actions are allowed. |

`configurationControl` determines whether local configuration writes, cloud
configuration Fetch, or both are permitted. `cloudConnection` controls cloud
communication but does not disable the local API or mDNS. During a committed
Stationary Wi-Fi OTA, Go keeps its available local endpoint read-only: GET
requests continue to return cached snapshots, while valid writes and actions
return `403 forbidden`.

See the [Go Local Server service doc](../products/go/docs/local_server.md) for
the complete Go lifecycle, configuration ranges, and asynchronous request
handling details.

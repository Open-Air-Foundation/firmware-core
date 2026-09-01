# Measurement Corrections

Measurement corrections derive a user-facing PM2.5, temperature, and humidity
view from authoritative raw AirGradient Go measurements. Corrections are
validated and persisted as device settings, while cloud, storage, and BLE
measurement transports carry the sensor values before user corrections.
Temperature and humidity on those transports are already compensated for
enclosure board heat (see `products/go/main/go_thermal_comp.h`); the raw SHT
reading passes through only while the airflow detector reports forced airflow
or the DPS368 temperature is invalid.

## Files

| File | Purpose |
|---|---|
| [`measurement_corrections.h`](../../../components/airgradient-common/include/measurement_corrections.h) | Correction algorithms, settings types, validation, comparison, and transform API |
| [`measurement_corrections.cpp`](../../../components/airgradient-common/measurement_corrections.cpp) | Pure correction math and runtime result validation |
| [`go_config_types.h`](../main/go_config_types.h) | Shared `GoConfigUpdate`, field mask, source identity, and source-control policy |
| [`go_cloud.cpp`](../main/go_cloud.cpp) | AirGradient cloud wire parsing into `GoConfigUpdate` |
| [`go_local_api.cpp`](../main/go_local_api.cpp) | Local API mapping, semantic validation, and translation into `GoConfigUpdate` |
| [`serial_command.cpp`](../main/serial_command/serial_command.cpp) | Manufacturing USB command parser and typed custom-correction request bridge |
| [`go_settings.cpp`](../main/go_settings.cpp) | Grouped correction persistence and boot-time loading |
| [`go_orchestrator.cpp`](../main/go_orchestrator.cpp) | Persist-before-activate updates and raw/corrected consumer routing |
| [`go_app.cpp`](../main/go_app.cpp) | Offline timer-wake fast-path correction |

## Dependencies

| Dependency | Source | Usage |
|---|---|---|
| `MeasuresAGo` | `airgradient-common` ([`measures_types.h`](../../../components/airgradient-common/include/measures_types.h)) | Raw input, corrected copy, field validators, ranges, and invalid sentinels |
| `ConfigStore` | `airgradient-config` | Persistent algorithm, coefficient, and PM EPA-flag storage |
| `GoSettings` | product ([`go_settings.h`](../main/go_settings.h)) | Complete active and persisted correction set |
| `CloudService` | product ([`go_cloud.cpp`](../main/go_cloud.cpp)) | Parses cloud correction fields after a successful complete fetch |
| `LocalServer` | `airgradient-local-server` ([`local_config.h`](../../../components/airgradient-local-server/types/local_config.h)) | Local Config JSON types and strict nested-field parsing |
| `Orchestrator` | product ([`go_orchestrator.cpp`](../main/go_orchestrator.cpp)) | Owns raw and corrected snapshots and routes each consumer |

## Public API

| API | Purpose |
|---|---|
| `MeasurementCorrections` | Complete PM2.5, temperature, and humidity correction configuration |
| `is_pm25_correction_valid()` | Accept only a supported PM2.5 enum and finite coefficients |
| `is_linear_correction_valid()` | Accept only a supported linear enum and finite coefficients |
| `are_measurement_corrections_valid()` | Validate the complete correction set before persistence or activation |
| `measurement_corrections_equal()` | Detect whether the corrected presentation must be recomputed |
| `apply_measurement_corrections()` | Copy a raw `MeasuresAGo` and replace only corrected PM2.5, temperature, and humidity fields |
| `GoConfigUpdate` | Queue-copyable partial update shared by cloud fetch and the local API adapter |
| `GET /api/v1/config` | Return the active corrections using local API names and canonical shapes |
| `PUT /api/v1/config` | Submit a partial local correction update for validation and queued application |
| `GET /api/v1/measures` | Return the latest corrected presentation snapshot |

See the shared [correction header](../../../components/airgradient-common/include/measurement_corrections.h),
the product [configuration types](../main/go_config_types.h), and the
[`airgradient-local-server` API](../../../components/airgradient-local-server/README.md)
for the complete interfaces.

## Behavior

### Supported Algorithms

Algorithm and property names are case-sensitive.

| Measure | Algorithm | Behavior |
|---|---|---|
| PM2.5 | `none` | Preserve valid raw PM2.5 |
| PM2.5 | `epa_2021` | Apply the EPA 2021 piecewise correction using raw averaged PM2.5 and raw averaged humidity |
| PM2.5 | `custom_via_pm25_raw` | Apply a linear scale and intercept to raw PM2.5 |
| Temperature | `none` | Preserve valid raw temperature |
| Temperature | `custom` | Apply `scaling factor * raw + intercept` in Celsius |
| Humidity | `none` | Preserve valid raw relative humidity |
| Humidity | `custom` | Apply `scaling factor * raw + intercept` |

The PM custom transform preserves an exact raw zero instead of adding the
intercept and clamps negative finite results to zero. Temperature conversion to
Fahrenheit and all presentation rounding happen after correction.

For the EPA transform, let `p` be PM2.5 and `h` be raw relative humidity clamped
to its valid range. The implemented piecewise equations are:

```text
p == 0:
  0

p < 30:
  0.524p - 0.0862h + 5.75

30 <= p < 50, x = 0.05p - 1.5:
  (0.786x + 0.524(1 - x))p - 0.0862h + 5.75

50 <= p < 210:
  0.786p - 0.0862h + 5.75

210 <= p < 260, y = 0.02p - 4.2:
  (0.69y + 0.786(1 - y))p - 0.0862h(1 - y)
  + 2.966y + 5.75(1 - y) + 8.84e-4p²y

p >= 260:
  2.966 + 0.69p + 8.84e-4p²
```

The final finite result is floored at zero. The source of truth remains
[`measurement_corrections.cpp`](../../../components/airgradient-common/measurement_corrections.cpp).
Tests exercise the boundary inputs and PM custom zero behavior; exact expected
EPA values are not asserted at every boundary.

### Wire Names and Shapes

The local and cloud APIs represent the same typed settings but intentionally
use different measure and PM scaling-factor names.

| Meaning | Local API | AirGradient Cloud |
|---|---|---|
| PM2.5 entry | `corrections.pm25` | `corrections.pm02` |
| Temperature entry | `corrections.temperature` | `corrections.atmp` |
| Humidity entry | `corrections.humidity` | `corrections.rhum` |
| PM custom scaling factor | `slr.scalingFactor` | `slr.scalingFactorViaPm25` |
| Linear custom scaling factor | `slr.scalingFactor` | `slr.scalingFactor` |
| Shared fields | `correctionAlgorithm`, `slr.intercept` | `correctionAlgorithm`, `slr.intercept` |

Local Config parsing is strict. Correction objects accept only `pm25`,
`temperature`, and `humidity`; each entry accepts only `correctionAlgorithm`
and `slr`; each `slr` object accepts only fields valid for that measure. Unknown
nested keys, including cloud aliases, reject the request.

Local API algorithm shapes are:

- `none` requires no SLR parameters. `slr` is absent or null, and GET emits it
  as null.
- PM2.5 `epa_2021` has the same no-SLR shape.
- PM2.5 `custom_via_pm25_raw` requires finite, float-representable
  `intercept` and `scalingFactor` numbers. The shared Local API schema accepts
  `useEpa2021`, but Go ignores it and omits it from GET responses.
- Temperature and humidity `custom` require finite, float-representable
  `intercept` and `scalingFactor` numbers in `slr`; `useEpa2021` is rejected.

The cloud parser tolerates unrelated root, correction-entry, and SLR fields,
but the supported values retain strict types and required names. Cloud PM2.5
custom input requires `scalingFactorViaPm25`; `scalingFactor` is not an alias.
Cloud custom inputs require all parameters, while `none` and `epa_2021` ignore
`slr`. Go ignores `useEpa2021` when it is present. A malformed cloud measure
leaves only that measure's update bit clear, so valid siblings remain
applicable.

See the local component
[`config_json.tests.cpp`](../../../components/airgradient-local-server/tests/config_json.tests.cpp),
the product [`go_local_api.tests.cpp`](../tests/go_local_api.tests.cpp), and
[`go_cloud.tests.cpp`](../tests/go_cloud.tests.cpp) for accepted and rejected
wire shapes.

### Configuration Updates and Persistence

Cloud parsing and local API translation both produce a `GoConfigUpdate`. Its
`update_mask` identifies the successfully supplied fields; missing fields keep
their current values. An explicit valid `none` entry selects the identity
configuration and is the command that disables a correction.

The orchestrator checks source-control policy, merges selected fields into a
candidate `GoSettings`, validates the complete candidate, and ignores an
unchanged candidate. For a changed candidate, `save_go_settings()` writes all
settings and commits `ConfigStore` before the orchestrator replaces its active
settings. Any validation, write, or commit failure leaves the previous active
corrections and corrected snapshot unchanged.

This activation is atomic only in memory. Multi-key `ConfigStore` persistence
is best-effort and non-transactional: a successful write that precedes a later
write or commit failure is not rolled back.

After successful activation, the orchestrator recomputes the corrected view
from the latest raw snapshot, refreshes the PM AQI LED and display, and
publishes updated Local Config and Local Measures snapshots. It does not emit a
BLE Measures notification because that transport remains raw.

Corrections load from `GoSettings` in every operating mode. Each measure loads
as a group: a missing or invalid algorithm, or an incomplete or non-finite
custom group, falls back independently to `none`. The other correction groups
remain usable. See [`go_settings.tests.cpp`](../tests/go_settings.tests.cpp) and
the orchestrator's
[`go_orchestrator.tests.cpp`](../tests/go_orchestrator.tests.cpp) for grouped
loading, commit failure, recomputation, and routing coverage.

### Runtime Validation

The transform starts with a copy of the raw `MeasuresAGo`; PM1, PM10, CO2,
TVOC, NOx, pressure, power, and all other fields remain unchanged. Before any
math, each target uses its field-specific validator and an explicit finite
check. Invalid or non-finite raw values become that field's existing invalid
sentinel in the corrected copy.

Temperature and humidity results must be finite and remain in their
field-specific valid ranges. Out-of-range results become invalid sentinels
rather than being clamped. PM2.5 results must be finite and are clamped only at
zero. When an EPA stage needs humidity and raw humidity is invalid, PM2.5 falls
back to the original valid raw PM2.5 instead of exposing a partial correction.

### Consumer Ownership

Raw measurements remain authoritative. Corrected measurements are temporary
presentation values and are not written back into `MeasuresAGo` storage
records.

| Consumer or State | Value Ownership | Behavior |
|---|---|---|
| Local API Measures | Corrected | Publishes the latest corrected snapshot |
| Current display | Corrected | Applies unit and AQI presentation after correction |
| Display charts | Corrected | Corrects raw cache copies in the chart scratch buffer at render time |
| PM AQI LED | Corrected | Converts corrected PM2.5 to AQI color |
| AirGradient cloud POST | Raw | Cloud receives the authoritative sensor snapshot |
| RTC measurement cache | Raw | Preserves chart samples without a correction version |
| Persistent route storage | Raw | Preserves route points without a correction version |
| BLE Measures | Raw | Clients choose whether to apply the correction settings |
| BLE History | Raw | Exports raw route-point copies for client-side policy |
| SGP41 temperature/humidity compensation | Raw | Compensation is independent of presentation correction |
| Sensor health and recovery logic | Raw | User correction cannot affect hardware diagnostics |
| RTC display snapshot | Corrected presentation | Seeds only the wake display view and never authoritative cloud or storage state |

See [Storage Service](storage_service.md#data-tiers) for cache and route
ownership. Raw BLE history export is verified in
[`go_ble.tests.cpp`](../tests/go_ble.tests.cpp).

### Offline Fast Path

The locked Offline timer-wake path loads persisted `GoSettings` before sensor
measurement and does not construct the cloud service or orchestrator. It keeps
the one-shot measurement raw for the RTC measurement cache and route append,
then `build_fast_path_display()` applies the same shared transform to a copy for
the e-paper frame.

If the fast path promotes to interactive operation, its boot handoff carries
the raw measurement. The orchestrator derives a fresh corrected view from that
raw value. A separate RTC display snapshot contains corrected presentation
state and can seed the display after wake, but it never seeds the authoritative
raw snapshot. See [`go_app.tests.cpp`](../tests/go_app.tests.cpp) for fast-path
presentation coverage and [`go_orchestrator.tests.cpp`](../tests/go_orchestrator.tests.cpp)
for raw handoff and RTC snapshot isolation.

## Edge Cases / Errors

- Unsupported or case-mismatched algorithms do not replace an active
  correction. Local API requests are rejected; cloud updates retain valid
  sibling measures.
- Missing correction entries are no-ops. Explicit `none` resets that measure
  to canonical identity parameters.
- Numeric strings, Booleans in numeric fields, non-finite values, and numbers
  outside finite `float` representation are rejected before persistence.
- A local correction entry with unknown nested fields is rejected. The cloud
  parser ignores unrelated fields but never treats a wrong required name as an
  alias.
- Invalid or incomplete persisted custom groups fall back to `none` per
  measure, without disabling valid sibling groups.
- A non-finite correction intermediate or output becomes the field's invalid
  sentinel. Temperature and humidity are not clamped into range.
- EPA correction with invalid raw humidity retains valid raw PM2.5. Custom PM
  with optional EPA also discards its partial linear result in this case.
- A settings write or commit failure retains the previous in-memory settings
  and corrected presentation.

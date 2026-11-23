# AirGradient Go Runtime Memory Experiments

> **Status: diagnostics added; memory reductions not applied.** Heap and stack
> checkpoints are authorized for baseline measurement. The agreed first reduction
> experiment is to set both BLE packet-pool counts to 12. Do not change firmware
> settings until implementation of the reductions is requested.

This specification records the memory investigation and proposed experiments
for AirGradient Go, covering runtime allocations and WiFi code placement in
SRAM. It separates measured baseline costs, proposed changes, and results that
still require hardware verification.

## Problem

Concurrent BLE, WiFi, and application services leave insufficient heap headroom
in the supplied device captures. The baseline firmware commit is `7213fae`.
The local capture `result.log.1` contains these observations; the capture itself
is not committed:

| Stage | Free Heap Before | Free Heap After | Decrease |
|---|---:|---:|---:|
| Boot phase 2 to phase 3 | 148,720 B | 124,760 B | 23,960 B |
| BLE initialization | 119,900 B | 84,748 B | 35,152 B |
| WiFi initialization | 79,808 B | 39,208 B | 40,600 B |
| Local HTTP activation | 35,268 B | 25,040 B | 10,228 B |
| Cloud start | 19,804 B | 9,128 B | 10,676 B |

The boot-phase decrease includes NAND mounting and other service construction;
it is not an isolated NAND measurement. Later, with HTTP and cloud services
active, minimum free heap reaches 220 bytes. A TLS certificate parsing failure
occurs during this sequence, but its error code alone does not prove an
allocation failure.

## Goals

- Reduce BLE's resident runtime allocation while preserving supported transfers.
- Evaluate freeing SRAM by moving optional WiFi code out of IRAM.
- Account for Cloud and Local Server stacks and additional heap use separately.
- Compare free heap, minimum free heap, and largest free block at matching stages.
- Verify pairing, OTA, history downloads, and coexistence with WiFi on hardware.
- Keep expected savings separate from measured results.

## Non-Goals

- Changing BLE MTU, buffer sizes, connection count, or application protocols.
- Changing WiFi or NAND allocation settings in the first experiment.
- Moving non-WiFi code between IRAM and flash or changing firmware partitions.
- Optimizing local HTTP or cloud allocations in this experiment.
- Updating general product documentation or VHUB during this planning step.

## Design

### Experiment 1: Reduce Both BLE Pool Counts

Apply these changes together when implementation is requested:

| Symbol | Baseline | Experiment |
|---|---:|---:|
| `CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT` | 24 | 12 |
| `CONFIG_BT_NIMBLE_MSYS_2_BLOCK_COUNT` | 24 | 12 |
| `CONFIG_BT_NIMBLE_MSYS_1_BLOCK_SIZE` | 128 | 128 |
| `CONFIG_BT_NIMBLE_MSYS_2_BLOCK_SIZE` | 528 | 528 |
| `CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU` | 512 | 512 |
| `CONFIG_BT_NIMBLE_MAX_CONNECTIONS` | 1 | 1 |

The nominal buffer capacity will decrease from 15,744 to 7,872 bytes, a saving
of 7,872 bytes (approximately 7.7 KiB). Actual heap savings may differ because
of pool metadata and allocation alignment. These pools are reserved while the
BLE stack is initialized; returning a buffer to its pool does not return that
memory to the general heap.

The existing implementation supports trying fewer buffers:

- [History streaming](../main/go_ble.cpp) submits one record per processing tick:
  59 bytes for tracking or 42 bytes for measurements, including its header.
  A failed binary notification submission leaves the record pending for retry.
- [BLE OTA](../../../components/airgradient-ota/docs/ble_push.md) accepts data
  writes up to the negotiated ATT payload, 509 bytes at MTU 512. It writes
  directly to flash from the host callback without an application chunk queue.
- Go supports one BLE connection. The baseline pool counts match the installed
  ESP-IDF defaults rather than an explicit Go application queue requirement.

These observations do not establish a safe minimum count. Accepted notifications
can remain queued in the stack, and incoming OTA packets can accumulate while
the host writes flash. OTA uses writes without per-chunk ATT responses, so
receive pressure and transfer integrity must be checked as well as throughput.

### Experiment 2: Move Optional WiFi Code Out of IRAM

Evaluate disabling WiFi's optional IRAM speed optimizations separately from
Experiment 1. All three options are enabled in the current Go configuration:

| Symbol | Baseline | Candidate | ESP-IDF Help Estimate |
|---|---|---|---|
| `CONFIG_ESP_WIFI_IRAM_OPT` | `y` | `n` | More than 10 Kbytes of IRAM |
| `CONFIG_ESP_WIFI_EXTRA_IRAM_OPT` | `y` | `n` | More than 5 Kbytes of IRAM |
| `CONFIG_ESP_WIFI_RX_IRAM_OPT` | `y` | `n` | More than 17 Kbytes of IRAM |

These estimates come from the installed ESP-IDF `components/esp_wifi/Kconfig`.
They are generic guidance, not measured savings for Go's ESP32-C5 build. Do not
sum them into a promised heap increase; compare the actual map and runtime heap.

IRAM holds executable code in SRAM. Disabling these options will place the
affected functions in flash for execution through the hardware instruction
cache. It will not load the entire application into RAM. Required IRAM routines
will remain governed by ESP-IDF's placement rules; do not manually relocate them.

This experiment targets static SRAM occupation, unlike Experiment 1's runtime
pool allocation. It may increase available heap from boot, including when WiFi
is off. It will not remove WiFi packet buffers, driver state, or task stacks.

The tradeoff is potentially lower WiFi throughput and greater execution latency
when instructions must be fetched from flash. Moving this code does not improve
execution speed. IRAM code already has a load image in the firmware binary, so
moving it does not imply an equal-sized increase in the binary. Section layout,
alignment, and linking can still change total image size; measure it explicitly.

When implementation is requested, first compare each option disabled separately
against the same reference configuration, then evaluate all three disabled
together. Keep BLE counts and WiFi buffer counts constant during those comparisons
and record their values. Measure static SRAM, mapped flash, total binary size,
remaining OTA-slot capacity, and free heap at matching boot and traffic stages.
Restore the relevant options to `y` if the performance or reliability tradeoff is
unacceptable. No configuration change is authorized by this planning document.

### WiFi Buffers: Allocation Behavior, No Agreed Change

Current settings use six static RX buffers, a limit of ten dynamic RX buffers,
and a limit of sixteen dynamic TX buffers. Static RX buffers are allocated at
WiFi initialization and retained until driver deinitialization.

Dynamic RX and TX buffers are allocated only as needed, with sizes dependent on
the frames being handled. An ongoing HTTP request does not allocate every buffer
up to those limits. Buffers are released as reception processing or transmission
completes; bursts and processing delays can increase simultaneous occupancy.
TCP and TLS allocations are separate from these WiFi driver buffers.

Reducing dynamic limits could constrain peak memory use, but would not provide
the same fixed baseline saving as shrinking BLE pools. Reducing static RX count
could save resident memory, but requires checking the receive aggregation window
and traffic behavior. Neither change is agreed for this experiment.

### NAND: Geometry-Dependent Buffers, No Agreed Change

The [NAND driver](../../../components/spi_nand_flash/src/nand.c) detects the chip
before allocating its work, read, and temporary buffers. Their sizes are one
physical page, one physical page, and one physical page plus one byte. It allocates
only the detected geometry, never both a 2 KiB and a 4 KiB set.

Those buffers total approximately 6 KiB for a 2 KiB-page chip or 12 KiB for a
4 KiB-page chip. The instrumented baseline now confirms 2,048-byte pages and
131,072-byte erase blocks on the captured device. Physical page size is a chip
property, not a memory-tuning setting. The configured FAT sector size does not
establish physical page size.

There is no simple agreed reduction to these driver buffers. Reuse or removal
would require investigating their lifetimes and correctness. Filesystem caches
are separate allocations and may offer future tuning opportunities; the current
FAT configuration allocates per-file caches dynamically when files are opened.
Neither driver refactoring nor cache tuning is included in Experiment 1.

### Cloud and Local Server: Heap and Stack Baseline

Task stacks are allocated from heap when tasks are created. The stack capacities
below are already included in the observed startup heap decreases; do not add
them to those decreases a second time. Allocated stack capacity is not measured
maximum stack usage. Neither task's worst-case stack headroom has been established.

| Service | Observed Startup Heap Decrease | Configured Task Stack | Other Known Allocation |
|---|---:|---:|---|
| Cloud | 10,676 B (10.4 KiB) | 8,192 B (8 KiB) | 2,048 B fetch buffer |
| Local HTTP | 10,228 B (10.0 KiB) | 6,144 B (6 KiB) | Server state and route registration |

[CloudService](../main/go_cloud.cpp) allocates its fetch buffer, completion
semaphore, and worker task at start, and releases them at stop. The 8 KiB stack
plus 2 KiB fetch buffer explains most of its observed startup decrease. The
remaining 436 bytes is a checkpoint difference, not an independently measured
breakdown of task and allocator overhead.

Cloud requests need additional heap after startup for HTTP/TLS, certificate and
cryptographic state, TCP buffers, and JSON processing. Current TLS configuration
uses the following content-buffer capacities:

| Symbol | Current Value | Meaning |
|---|---:|---|
| `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN` | 4096 | Incoming TLS content capacity |
| `CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN` | 4096 | Outgoing TLS content capacity |
| `CONFIG_MBEDTLS_DYNAMIC_BUFFER` | Disabled | Optional dynamic TLS buffer mode is off |

These are not the total TLS memory requirement, nor allocations reserved merely
by starting CloudService. The capture falls from 9,128 bytes free before the first
TLS request to 4,032 bytes at a concurrent checkpoint, and later reports a
220-byte historical minimum. As other tasks run concurrently, do not attribute
every byte of that decrease exclusively to TLS.

[LocalServer](../../../components/airgradient-local-server/README.md) does not
create a separate task. Its handlers run on the
[HTTP driver's](../../../components/airgradient-http-server/drivers/idf_http_server.cpp)
6 KiB server task stack. Request bodies, JSON parsing/serialization, responses,
and socket activity can add transient heap use beyond activation. The measured
activation decrease is not the maximum cost of serving requests. mDNS starts
after the post-activation checkpoint and is outside the quoted 10,228-byte cost.

Potential later work is to measure stack high-water marks during successful and
failed cloud requests and demanding local requests before choosing smaller
stacks, and to profile buffer lifetimes and allocation peaks. No reduced stack
size, TLS setting, fetch-buffer size, or server limit has been selected.

### Main Task: Reserved Stack Versus Function Usage

| Symbol | Current Value | Meaning |
|---|---:|---|
| `CONFIG_ESP_MAIN_TASK_STACK_SIZE` | 16384 | Main task reserves 16 KiB of stack RAM |

The main task runs [app_main](../main/main.cpp), application startup, and then
the orchestrator's event loop. It remains alive during normal interactive
operation. ESP-IDF reserves its configured stack when creating the task; it
does not allocate a new heap buffer each time a function is called.

A function's executable instructions and its stack frame are different things.
The instructions for [GoApp::execute_fast_path](../main/go_app.cpp) remain in
the firmware's flash code whether or not that boot uses the function. Its stack
frame holds temporary data and saved call state only while the call is active.
Calling it uses space inside the main task's existing stack reservation.

`execute_fast_path()` handles a scheduled measurement wake from deep sleep:
sensor acquisition, temporary cache/chart updates, optional persistent
measurement logging, and display updates as needed. The surrounding wake path
normally returns Go to sleep, or hands off to interactive operation when needed.
It does not create a separate measurement task or a separate 3 KiB heap buffer.

Stack space follows the active call chain:

- Before a function is called, it has no active stack frame.
- While it runs, its frame and those of its callers occupy the same task stack.
  Nested calls need additional space. Blocking the task does not release these
  frames because execution must later resume with their state intact.
- When a function returns, its frame's space becomes reusable by subsequent
  calls on that task. This does not return any of the reserved stack to the heap.
- A caller that never returns can retain its frame for the task's lifetime.
  Deep sleep ends the current main-task execution; wake boots a new instance.

For example, a call using roughly 3 KiB inside a 16 KiB task stack does not
increase the task's reservation to 19 KiB. After the call returns, those 3 KiB
can be reused by other functions, but all 16 KiB remains unavailable to general
heap allocations while the task exists. Stack capacity does not grow on demand;
exceeding it is an overflow, not a request for more heap.

Compiler reports generated with the current ESP32-C5 compile settings and
`-fstack-usage` provide these partial observations:

| Function or Active Chain | Compiler-Reported Stack |
|---|---:|
| `app_main` + `GoApp::run` + `run_interactive` + `Orchestrator::run` | 2,416 B combined |
| `GoApp::run_interactive` alone, included in the preceding subtotal | 1,648 B |
| `GoApp::execute_fast_path` alone | 3,136 B |
| `Orchestrator::update_display` alone | 784 B |
| `OtaUpdater::run` alone | 1,088 B |

These are compiler frame sizes, not measured worst-case task usage. Do not add
all rows: some overlap, and other functions run on different paths. The
measurement-wake frame includes temporary measurement/chart data. The interactive
setup frame stays active while its call to the event loop runs. Nested library
calls and additional callers are not included in the individual-function rows.

Commit `3e2e082` raised the main stack from 3,584 to 16,384 bytes while addressing
stack overflow and moving persistent objects onto the heap. Its description
does not establish that the resulting implementation requires the full 16 KiB.

Reducing a function's frame can reduce peak stack demand, but only lowering the
configured task stack releases that capacity for other allocations. Moving local
data onto the heap is not automatically a net RAM saving. A possible later
experiment is 12 KiB, saving 4 KiB; 8 KiB would save 8 KiB but is not established
as safe. Neither value is selected or applied. First measure remaining stack
across cold boot, measurement wake and handoff, provisioning, storage operations,
and BLE/WiFi OTA, then retain margin above the largest observed requirement.

## Implementation Plan

1. Wait for an explicit request to implement the experiment.
2. Change both pool counts in Go's `sdkconfig.defaults` and tracked `sdkconfig`.
   Regenerate configuration and verify any generated compatibility aliases.
3. Build Go with the installed ESP-IDF environment sourced in the same shell.
4. Confirm both effective counts are 12 and buffer sizes and MTU are unchanged.
5. Record build results here, then have the user perform hardware verification.
6. Record measured savings and transfer results before deciding to retain or
   revise the experiment. Restore both counts to 24 if the experiment is rejected.
7. When Experiment 2 is requested, apply its isolated code-placement comparisons
   in Go's configuration files, build each variant, and record size and heap
   results separately from Experiment 1.

## Testing Strategy

### Diagnostic Capture

The shared `log_heap()` helper retains the existing `[HEAP]` prefix and appends
internal-memory availability and the calling task's stack watermark:

| Field | Meaning |
|---|---|
| `free` | Current total free heap reported by ESP-IDF |
| `min` | Historical minimum free heap reported by ESP-IDF for this boot |
| `largest` | Largest currently available allocation with default capabilities |
| `internal` | Current free internal, byte-addressable heap |
| `internal_largest` | Largest currently free block with those internal capabilities |
| `task` | Task executing this checkpoint, such as `main`, `cloud`, or `httpd` |
| `stack_min` | Historical minimum untouched stack of that task, in bytes |

`stack_min` is ESP-IDF's stack high-water-mark reading, already expressed in
bytes. Do not multiply it by a FreeRTOS word size. It is not current free stack
or a measurement of every task. The watermark scans the stack fill pattern;
untouched reserved frame space can make it optimistic, so compare it with
compiler frame sizes and leave margin. Log formatting itself consumes stack and
time, and the next checkpoint can reflect that cost. Heap fields are sequential
snapshots while other tasks may continue allocating, not an atomic profile.

Existing checkpoints cover boot phases, BLE/WiFi initialization, provisioning,
Local Server activation, Cloud start/stop and requests, WiFi OTA, periodic BMS
polls, and sleep preparation. Added checkpoints cover:

- Display object construction, driver/bounce-buffer initialization, initial
  rendering, worker startup, and worker refresh completion.
- Storage/cache construction, NAND device initialization separately from FAT
  mounting, and detected physical NAND page/block sizes.
- Tracking and measurement-log file creation/resume/close, exposing allocations
  while files remain open without logging each record append.
- BLE teardown and history export start, pass completion, and release, without
  per-packet logging.
- mDNS initialization and stop, separate from Local Server activation.
- HTTP requests before/after the handler and after response sending. The final
  checkpoint still includes request/response objects, before their destructors.
- BLE OTA entry/return and the return from the measurement-wake function. The
  latter retains the task watermark even after that function's frame is released.

Capture the existing periodic checkpoints after ordinary operation as well as
the transition logs. HTTP requests and display refreshes each produce diagnostics;
these instrumented timings are not final production throughput measurements.
No heap tracing buffers, extra sampling task, or buffer/stack size changes are
introduced. Flashing and capture remain user-only.

### Experiment Verification

These configuration-only experiments do not change application logic. A firmware
build checks configuration integration; host mocks cannot establish packet-pool
capacity or real transfer behavior. Flashing and serial monitoring remain
user-only.

Compare baseline and experiment using the same device, settings, wake path,
client, and network conditions. Start a fresh boot for each comparison because
minimum free heap is historical for that boot.

1. Capture free heap, minimum free heap, and largest free block before and after
   BLE initialization, then at matching WiFi and cloud checkpoints.
2. Pair a client, discover services, read configuration, and receive live data.
3. Download all tracking history and measurement logs. Check record counts,
   contents, retries, and elapsed time against the baseline.
4. Complete BLE OTA with the same image and client as the baseline. Check transfer
   completion, image validation, elapsed time, and boot of the updated firmware.
5. Exercise downloads and normal BLE activity with WiFi and the configured
   network services active. Exercise OTA under its existing product policy.
6. Repeat disconnect, reconnect, lock, and unlock cycles. Check for allocation
   failures, transfer stalls, resets, and progressive heap loss.

Retain the experiment only after observing lower BLE allocation and successful
functional checks. Record any throughput regression for review. A successful
build or a nominal 7.7 KiB saving alone does not establish that the original
low-heap failure is resolved.

For Experiment 2, also compare WiFi connection/reconnection, cloud HTTPS success
and request duration, Local Server responses under repeated requests, and WiFi
OTA completion and duration. Exercise simultaneous BLE traffic under the existing
product policy, including flash writes during OTA. Record throughput, timeouts,
resets, and heap/stack headroom where instrumentation is available. Compare binary
size with the actual OTA partition capacity for every build.

## Results and Open Questions

### Instrumented Baseline: result.base.log

The user-supplied `result.base.log` is the baseline before applying reductions.
It contains 82 heap checkpoints across an initial interactive boot and a later
button wake. Early boot output is incomplete. The initial run enters deep sleep;
the second run enables WiFi with BLE initialized, then starts Local Server,
mDNS, and Cloud. Measurement logging is active. No settings were changed during
analysis: both BLE pool counts remain 24, main stack remains 16 KiB, and all
three WiFi IRAM options remain enabled in the local configuration.

Capture SHA-256:
`8d2f730a9cda24b8fc10271ed4a8877334f5f2afec59f23a1a7f9b1e271e31a7`.
The raw capture is kept locally and is not included in the specification commit.
The file does not contain a complete app identity banner; configuration values
above describe the local build used for the user-designated baseline.

Matched checkpoints in the button-wake run give these heap decreases:

| Stage | Free Before | Free After | Decrease |
|---|---:|---:|---:|
| Storage/cache construction and restoration | 148,720 B | 146,708 B | 2,012 B |
| NAND SPI-device registration | 146,708 B | 146,060 B | 648 B |
| NAND device initialization | 146,060 B | 139,640 B | 6,420 B |
| FAT mount | 139,640 B | 135,000 B | 4,640 B |
| Measurement-log file resume | 122,276 B | 120,004 B | 2,272 B |
| BLE initialization | 119,900 B | 84,748 B | 35,152 B |
| WiFi initialization | 79,808 B | 39,220 B | 40,588 B |
| Local HTTP activation | 35,236 B | 25,008 B | 10,228 B |
| mDNS initialization | 25,008 B | 20,360 B | 4,648 B |
| Cloud start | 19,808 B | 9,132 B | 10,676 B |

NAND SPI registration, device initialization, and FAT mount total 11,708 bytes
(11.4 KiB). The same three deltas occur in the initial boot. This replaces the
earlier rough attribution of the whole 23,960-byte boot-phase decrease to NAND:
the button-wake phase also includes 2,012 bytes for storage/cache construction
and a further 10,240 bytes of subsequent service construction. These checkpoint
differences are not allocation traces, but the repeated NAND deltas strengthen
their attribution. mDNS service registration follows its initialization
checkpoint and is not fully included in its 4,648-byte figure.

The first cloud POST begins with 9,132 bytes free and a largest block of 6,656
bytes. Certificate parsing fails with `-0x3F80`, defined as
`MBEDTLS_ERR_PK_ALLOC_FAILED` in the installed mbedTLS `include/mbedtls/pk.h`.
Unlike the older capture's `-0x3B00`, this explicitly reports an allocation
failure. After the failed POST, free heap is 7,824 bytes, historical minimum is
156 bytes, and the largest free block is 1,792 bytes. Later configuration and
OTA-availability requests time out; WiFi subsequently loses the connection and
retries. The capture does not establish that memory pressure caused every later
network failure.

Recorded stack watermarks across both runs are:

| Task | Configured Capacity | Lowest Remaining | Capacity Minus Watermark |
|---|---:|---:|---:|
| `main` | 16,384 B | 10,988 B | 5,396 B |
| `cloud` | 8,192 B | 4,540 B | 3,652 B |
| `disp_worker` | 4,096 B | 3,180 B | 916 B |
| `sys_evt` | 2,304 B | 1,244 B | 1,060 B |

The main-task minimum comes from the initial interactive run; the second run's
minimum is 11,276 bytes remaining (5,108 bytes touched). Do not treat historical
watermarks as continuous across the deep-sleep reboot. These readings support
investigating a smaller main stack but do not establish worst-case requirements.
For the observed 5,396-byte footprint alone, 12 KiB would leave 6,892 bytes and
8 KiB would leave 2,796 bytes; these are arithmetic projections, not verified
headroom after a configuration change.

This capture does not exercise a scheduled measurement fast-path wake, completed
BLE/WiFi OTA, history export, successful cloud HTTPS, or Local Server request
handlers. It contains no `httpd` stack reading and no full display-initialization
checkpoint pair. Those paths remain required before final stack reductions;
the cloud watermark in particular covers a failed TLS path, not successful TLS.

### Verification Status and Remaining Decisions

- Reduction experiment configuration: not applied.
- Diagnostic Go and reference firmware builds: passed. Existing host suite:
  1,760 tests passed. The instrumented baseline capture has been analyzed above;
  broader hardware-path coverage remains pending.
- Instrumented Go binary: 1,943,696 bytes, up 2,080 bytes from the recorded
  baseline; 87,920 bytes remain in the smallest OTA slot. Diagnostics consume
  some flash and execution time; compare subsequent experiments with this
  instrumented baseline.
- Measured heap saving and transfer performance: unknown.
- Whether both pools can remain at 12 under OTA and concurrent WiFi: unverified.
- WiFi IRAM options: candidate experiment documented; all remain enabled.
- Actual IRAM/heap saving, binary-size change, and WiFi performance: unmeasured.
- Cloud and Local Server: startup costs and stack capacities documented;
  baseline Cloud watermark captured; Local Server handler stack use and
  worst-case request allocation peaks remain unmeasured.
- Main task: stack reservation and function-frame lifetimes documented, with
  partial compiler frame sizes and baseline watermarks; a safe reduced stack
  capacity remains unverified.
- Whether WiFi buffers, filesystem caches, or service stacks need further tuning:
  decide after measurement; no replacement values have been selected.

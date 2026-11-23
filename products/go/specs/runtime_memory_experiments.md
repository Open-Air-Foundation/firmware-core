# AirGradient Go Runtime Memory Experiments

> **Status: combined WiFi IRAM experiment confirms 13.25 KiB more boot heap.**
> The device capture still shows critically low heap during TLS and unsuccessful
> cloud requests. BLE counts of 12 showed no initialization saving; counts have
> returned to 24 and further BLE investigation is deferred. The user's 8 KiB
> main-stack capture confirms another 8 KiB of heap, with partial stack-path
> coverage. Apply further configuration changes only when requested.

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

- Evaluate BLE memory limits and allocation behavior while preserving transfers.
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

The nominal buffer budget decreases from 15,744 to 7,872 bytes. This is not a
fixed 7,872-byte heap saving. Investigation of the built ESP32-C5 controller
path shows that, with `CONFIG_BT_NIMBLE_MSYS_BUF_FROM_HEAP=y`, these buffers
allocate on demand. The counts and aligned sizes set a combined byte budget;
they do not reserve all blocks at initialization. Reducing counts constrains
peak buffer allocation but does not free memory that was never allocated.
This corrects the initial assumption that both pools were fully reserved.

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
Experiment 1. All three options are enabled in the baseline Go configuration:

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
a fixed baseline saving; the current C5 MSYS mode is also demand-allocated.
Reducing static RX count
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

| Symbol | Baseline Value | Meaning |
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
experiment is 12 KiB, saving 4 KiB. The user has now tried 8 KiB, recovering
8 KiB as recorded below; this is not established as safe for every path. Measure remaining stack
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

Retain the experiment only after establishing a useful reduction in peak BLE
allocation and successful functional checks. Record any throughput regression
for review. A successful build or a smaller configured budget alone does not
establish that the original low-heap failure is resolved.

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

### Experiment 2: Three WiFi IRAM Options Disabled

The user disabled `ESP_WIFI_IRAM_OPT`, `ESP_WIFI_EXTRA_IRAM_OPT`, and
`ESP_WIFI_RX_IRAM_OPT` in Go's `sdkconfig` and built the firmware. Generated
configuration confirms all three are disabled and `ESP_WIFI_SLP_IRAM_OPT`
remains enabled. This is a combined experiment; individual-option savings have
not been measured. `sdkconfig.defaults` has not been changed for this experiment.

For comparison, the committed `04ac638` configuration was rebuilt in a separate
temporary build directory using the same source and installed ESP-IDF, leaving
the user's configuration and build artifacts untouched. Its binary size matches
the recorded instrumented baseline.

| Metric | Instrumented Baseline | Three Options Off | Change |
|---|---:|---:|---:|
| IRAM text | 137,692 B | 124,078 B | -13,614 B |
| Static data | 18,260 B | 18,260 B | 0 B |
| Static BSS | 27,928 B | 27,928 B | 0 B |
| Flash-mapped text | 1,444,190 B | 1,457,278 B | +13,088 B |
| Complete binary | 1,943,696 B | 1,943,088 B | -608 B |

The linked heap-start address moves down by 13,568 bytes (13.25 KiB), accounting
for section alignment. The device capture below confirms this increase at matching
boot checkpoints. The binary gets slightly smaller
because placement also changes linking/layout; IRAM code already had a flash
load image. The combined IRAM reduction is substantially below the sum of generic
menuconfig estimates. The runtime saving does not establish sufficient memory
for successful TLS or acceptable connectivity under load.

`ESP_WIFI_SLP_IRAM_OPT` optimizes connected WiFi power-saving operation: expected
beacon timing (TBTT), beacon reception, buffered-traffic indication processing,
and related sleep/wakeup work. It retains both sleep-specific and shared receive
sections in IRAM even when `ESP_WIFI_RX_IRAM_OPT` is disabled. The new map retains
1,948 bytes in `.wifislpiram` and 7,476 bytes in `.wifislprxiram`, about 9.2 KiB
combined. This is not the option's complete marginal cost: it also selects
power-management/peripheral/PHY options whose placement and defaults must be
evaluated separately.

Keeping this work in IRAM aims to shorten active processing around wakeups and
reduce average current in WiFi power-save mode. It is relevant when Go keeps
WiFi connected with Local Server off, where current policy selects MAX_MODEM.
It is distinct from the deep-sleep measurement-wake path. Disabling this option
would not itself switch off modem-sleep, but can change processing overhead,
power consumption, and related selected power-management settings. Keep it
enabled for the present comparison; any further experiment should measure power
as well as memory and connectivity.

#### Device Capture

The user supplied `result.wifi-iram.log` (712 lines, SHA-256
`4ec0a6990421f579fd518484282cb36a6642572e39e99376c8da5199408a7e65`).
Compare it with the instrumented `result.base.log` capture above. Values below
are free heap in bytes:

| Checkpoint | Baseline | Three Options Off | Change |
|---|---:|---:|---:|
| Cold boot, storage initialized | 149,548 | 163,116 | +13,568 |
| Cold boot, before interactive run | 115,416 | 128,984 | +13,568 |
| Button wake, phase 3 complete | 124,760 | 138,328 | +13,568 |
| WiFi initialized | 39,220 | 53,344 | +14,124 |
| First cloud POST, before TLS | 9,132 | 22,436 | +13,304 |

The exact boot increase confirms the linker prediction. Later checkpoints vary
with timing and concurrent allocations; they are not identical workloads.
WiFi initialization itself consumes 40,588 bytes in both captures. Moving code
out of IRAM has not reduced that runtime allocation. Cloud startup still consumes
10,676 bytes.

The additional heap is insufficient to establish a healthy operating margin:

- Minimum-ever free heap reaches 1,588 bytes, versus 156 bytes in the baseline.
- During a config-fetch TLS attempt, a checkpoint records 2,456 bytes free and
  a largest free block of only 1,024 bytes. Total free heap is not a guarantee
  that a larger contiguous allocation can succeed.
- The first cloud POST times out. Two config-fetch attempts report `-0x004C`
  immediately after beacon timeout and WiFi disassociation. Installed mbedTLS
  defines this as `MBEDTLS_ERR_NET_RECV_FAILED`, a socket-read failure, rather
  than the baseline's explicit `-0x3F80` allocation failure.
- No successful cloud POST or config fetch appears in this capture. Heap returns
  to roughly 23 KiB after TLS cleanup and 27 KiB after mDNS stops; that recovery
  does not establish sufficient headroom during a handshake.
- Beacon timeout also occurred in the baseline. These captures cannot attribute
  the connectivity failure to flash execution, memory pressure, or the network.
  A controlled comparison is still required before accepting WiFi reliability.

This capture confirms the memory saving, but does not validate throughput,
power consumption, OTA, or successful concurrent network workloads. Main-task
stack minimum remains 10,988 bytes on the cold-boot path and 11,276 bytes on
the button-wake path; it adds no evidence for a lower worst-case stack capacity.

### Experiment 1: BLE Pool Counts Built at 12

Following the WiFi IRAM capture, the user reduced both
`CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT` and
`CONFIG_BT_NIMBLE_MSYS_2_BLOCK_COUNT` from 24 to 12 and rebuilt. Pool block
sizes remain 128 and 528 bytes. Generated configuration confirms these values.
Disassembly of `os_msys_init` independently confirms the built ELF passes
`128, 528, 12, 12, 1` to `r_esp_ble_msys_init`; the final argument selects heap
allocation. The baseline passes counts of 24.

Relative to the preceding WiFi-only experiment, the linker map is unchanged,
including static SRAM use of 170,266 bytes. The binary still occupies 1,943,088
bytes, although its contents have changed. This is expected for a change to a
runtime allocation budget rather than static SRAM or executable code size.
The earlier prediction of 7.69 KiB more heap at BLE initialization, or 20.94 KiB
combined with the WiFi change, was incorrect for this controller configuration.

The three WiFi IRAM options remain disabled and SLP remains enabled, so this is
an incremental BLE experiment on top of the WiFi change. `sdkconfig.defaults`
still specifies BLE counts of 24. BLE history/log downloads and OTA remain
unverified at these counts.

#### Device Captures and Allocator Correction

The initial `result.ble-pool.log` capture showed no initialization saving. The
user explicitly re-flashed and supplied `result.ble-pool.log.1` (682 lines,
SHA-256 `bb9cfa689957d8554ae710843a95e4078d3678ca1fd2a40d2640b3c813f6ad9d`).
All three captures have exactly the same BLE initialization checkpoints:

| Metric | WiFi-Only Capture | BLE Counts 12 | Re-Flashed Counts 12 |
|---|---:|---:|---:|
| Free heap before BLE initialization | 133,572 B | 133,572 B | 133,572 B |
| Free heap after BLE initialization | 98,424 B | 98,424 B | 98,424 B |
| Initialization decrease | 35,148 B | 35,148 B | 35,148 B |

The installed `components/bt/porting/mem/os_msys_init.c` delegates initialization
to the controller because `CONFIG_BT_LE_MSYS_INIT_IN_CONTROLLER=y`.
Disassembly of `r_ble_ll_mem_msys_init` shows the heap-source branch initializes
empty pools, computes the combined budget from counts and aligned sizes, and
registers allocation callbacks. `r_ble_ll_mem_memblock_get_cb` checks that
budget through `r_ble_ll_mem_msys_alloc_check` before allocating a requested
block. Thus unchanged initialization heap is consistent with the compiled
allocator; it is not evidence that the device was running the old build.

The re-flashed capture reaches 1,632 bytes minimum-ever free heap. Its lowest
sampled free heap is 2,508 bytes and its smallest sampled largest block is
1,024 bytes. Cloud POST times out, config fetches fail with socket-read error
`-0x004C` after beacon timeouts, and no successful cloud request appears.
The earlier BLE capture additionally reports `-0x2880`, which installed
mbedTLS defines as `MBEDTLS_ERR_X509_ALLOC_FAILED`. The last re-flashed fetch
is still in progress when the capture ends.

No fixed idle heap benefit or resolution of the TLS pressure is demonstrated.
The confirmed baseline gain remains the WiFi change's 13.25 KiB. Whether a
lower MSYS budget usefully limits peaks during actual BLE transfers, without
breaking those transfers, remains unverified.

### Experiment 3: User's 8 KiB Main-Stack Capture

The user reduced `CONFIG_ESP_MAIN_TASK_STACK_SIZE` to 8,192 bytes and supplied
`result.main-task.log` (435 lines, SHA-256
`72c6c5a2f76b6b8755cd1a19c81d3ecb6b18b5393fe35d44399741456a653d84`).
Local and generated build configuration agree on 8 KiB; both BLE MSYS counts
are 24 and the preceding WiFi IRAM changes remain in place.

Compared with `result.wifi-iram.log`, matching button-wake checkpoints confirm
an exact 8,192-byte heap increase:

| Checkpoint | Main Stack 16 KiB | Main Stack 8 KiB | Change |
|---|---:|---:|---:|
| Button wake, phase 3 complete | 138,328 B | 146,520 B | +8,192 B |
| Before BLE initialization | 133,572 B | 141,764 B | +8,192 B |
| After BLE initialization | 98,424 B | 106,616 B | +8,192 B |

This establishes 21,760 bytes (21.25 KiB) of combined heap improvement from the
WiFi code-placement and main-stack changes at matching button-wake checkpoints
before BLE initialization, relative to the original instrumented baseline.
BLE initialization still costs 35,148 bytes in this capture.

The main-task minimum remaining stack is 2,796 bytes during cold-boot logging
and sleep preparation, corresponding to 5,396 bytes touched. On the subsequent
button-wake interactive path it remains at 3,084 bytes (5,108 bytes touched).
These match previous usage with the smaller reservation. No stack overflow or
panic appears, but scheduled measurement wakes and handoff, provisioning, OTA,
and other unexercised paths still need coverage before accepting 8 KiB broadly.

Before the first cloud POST, free heap is 31,240 bytes with a 20,480-byte largest
block. After failed TLS attempts, free heap recovers to roughly 30 KiB. The
minimum-ever free heap nevertheless reaches 1,336 bytes, so transient memory
pressure remains. The sampled largest blocks after cleanup do not show the
smallest contiguous block during the handshake.

All four completed cloud POST/config-fetch attempts report `-0x2700`, defined
by installed mbedTLS as `MBEDTLS_ERR_X509_CERT_VERIFY_FAILED`. This differs from
the earlier allocation and socket-read errors; the capture does not provide
verification flags identifying the certificate failure's underlying cause.
Do not infer that memory pressure is resolved or that device time or a CA is
necessarily responsible. No successful cloud request appears. The OTA
availability request receives HTTP 400; its configured URL uses HTTP, so this
does not validate HTTPS. No beacon timeout or WiFi disconnection appears in
this shorter capture; that is not a complete connectivity qualification.

### Allocation-Failure Callback

The user requested direct allocation-failure evidence after the 8 KiB main-stack
capture. Go now calls `enable_heap_failure_logging()` before constructing the
board in `app_main`. The shared platform helper registers ESP-IDF's process-wide
failed-allocation callback; it is a no-op under `TEST_HOST`. Startup logs confirm
registration. It observes failures after registration, not earlier boot work.

Each failure emits a `[HEAP_FAIL]` line with these fields:

| Field | Meaning |
|---|---|
| `size` | Requested allocation bytes |
| `caps` | Requested ESP-IDF memory-capability bitmask, in hexadecimal |
| `function` | Allocator function reporting the failure, not the application caller |
| `task` | Calling task name, or `ISR` for interrupt context |
| `free` | Current free bytes matching the requested capabilities |
| `largest` | Largest current free block matching those capabilities |

The callback uses ROM output and heap queries without allocating a message
buffer, creating a task, scanning stack watermarks, or invoking the normal
logging backend. Free and largest-block queries are separate snapshots and can
change under concurrent activity. Serial output can affect timing on failure;
this is diagnostic instrumentation, not a memory fix. It does not enable
abort-on-allocation-failure or change configuration.

Repeat the cloud scenario and correlate `[HEAP_FAIL]` lines with TLS errors.
An allocation failure in the cloud task is evidence of heap pressure even if
the higher-level TLS result is certificate verification failure. It does not
alone prove that a particular failed allocation caused that TLS result, since
some callers can recover or retry. Absence of the callback cannot exclude
exhaustion in a separate fixed pool that does not call ESP-IDF's heap allocator.

Verification: Go and reference firmware builds passed; all 1,760 existing host
tests passed, along with Markdown lint and whitespace checks. Actual callback
output is confirmed in the user-supplied excerpt described below.

### Experiment 4: Cloud and TLS Memory Reduction

The user set both TLS content limits to 2,048 bytes through configuration and
authorized the remaining code changes. Initial values for this experiment were:

| Setting | Baseline | Experiment | Owner |
|---|---:|---:|---|
| Cloud task stack | 8,192 B | 6,144 B | `go_cloud.cpp` |
| Config-response buffer | 2,048 B | 1,024 B | `go_cloud.cpp` |
| Go POST body buffer | 1,024 B | 512 B | `ag_client.cpp`, Go model only |
| TLS RX content limit | 4,096 B | 2,048 B | User's `sdkconfig` |
| TLS TX content limit | 4,096 B | 2,048 B | User's `sdkconfig` |

Other products keep their 1,024-byte POST buffer. This experiment does not
change HTTP RX/TX buffers, BLE, WiFi packet buffers, SLP IRAM, or NAND settings.
Further BLE investigation is deferred, and the user excluded SLP and NAND from
further reductions. Product documentation and VHUB consolidation remain deferred;
this specification records the experiment.

The Cloud stack and response-buffer reductions should release 3,072 bytes while
Cloud is started, reducing the previously observed 10,676-byte startup cost to
approximately 7,604 bytes. The two TLS buffer changes should release another
4,096 bytes while a connection has both buffers allocated, for approximately
7 KiB combined relative to Experiment 3. The smaller POST body reduces stack
demand by 512 bytes, not an additional heap reservation. Runtime gains remain
to be measured; these figures are not guarantees that TLS will complete.

The proposed 5 KiB Cloud stack was discussed but not selected. The largest
previously observed usage is 3,652 bytes, leaving only 1,468 bytes at 5 KiB,
before covering successful TLS and config parsing. The 6 KiB trial leaves
2,492 bytes above that observed usage. The smaller POST frame helps that path
but does not lower every call chain's requirement.

The user reports config GET responses below 500 bytes. The new response buffer
accepts up to 1,023 bytes plus the terminator. Existing truncation handling
returns `BufferTooSmall` and avoids applying partial configuration. The boundary
test applies a padded 1,023-byte configuration and rejects a larger response.
A serializer test checks that all 23 Go fields with large representative values
and maximum uptime fit in 512 bytes; this is not an exhaustive upper bound for
all values accepted by the shared measurement types. Serialization failure
continues to prevent transmission rather than sending truncated JSON.

The allocation-failure callback confirms that the preceding TLS attempts ran
out of heap. In the supplied POST excerpt, Cloud requests 2,584 bytes with
1,220 bytes free and an 832-byte largest block. WiFi also fails to allocate
1,856 bytes. During config fetch, another 2,584-byte Cloud request fails with
1,772 bytes free; the network task fails a 257-byte request with only 64 matching
bytes free. TLS returns `-0x2700` after these failures. The callback establishes
heap pressure even though the reported TLS result is certificate verification.

After the user flashes this experiment, repeat POST, config GET, and OTA
availability checks, recording `[HEAP_FAIL]`, minimum heap, and Cloud stack
watermarks. Smaller TLS RX capacity must also accommodate server handshake
records, including certificates; a sub-500-byte JSON body alone does not establish
that the new limit is compatible. No on-device acceptance is claimed yet.

The subsequent `result.cloud.log` capture with RX/TX both at 2 KiB reports
TLS `-0x7100` for all four completed POST/fetch attempts, with no allocation
failure callback and a 10,504-byte minimum heap. This does not establish that
the handshake fits; it aborts before successful communication.

The user restored RX to 4,096 bytes, retaining TX at 2,048 bytes. This is the
configuration committed in baseline `860e485`. In `result.cloud.log.1`, Cloud
startup costs 7,604 bytes, confirming the 3,072-byte reduction. The first TLS
attempt starts with 34,336 bytes free, but allocation failures recur and the
minimum-ever heap reaches 264 bytes. All four completed POST/fetch attempts
fail. Cloud's minimum untouched stack is 3,076 bytes out of 6,144 reserved;
successful requests and OTA still need coverage. With only TX reduced, the
expected TLS buffer saving is 2,048 bytes, rather than the initial 4,096.

### Local Server and mDNS Usage Measurements

After committing the current experiment baseline, add named task stack
watermarks without initially changing stack sizes, buffers, or configuration.
The measurement baseline reserves 6,144 bytes for Local HTTP and 4,096 bytes
for mDNS. Existing startup heap deltas
include these reservations plus service allocations; they are not stack usage.

Go's existing 30-second BMS timer logs `[STACK]` entries for `httpd` and `mdns`,
including when no HTTP request is in progress. Startup logs report reserved stack sizes.
The HTTP driver also samples its task after startup and before shutdown, and
the WiFi driver samples mDNS after profile configuration and before normal
mDNS shutdown. HTTP shutdown gets before/after heap snapshots; mDNS already
has them. A new `mdns.start:configured` heap snapshot includes the profile
setup following `mdns.init:post`.

`stack_min` is the minimum untouched stack in bytes since that task was created.
Subtract it from the reserved size to estimate the maximum stack touched so far:
`6144 - stack_min` for HTTP and `4096 - stack_min` for mDNS. A stopped task logs
`state=absent`; it does not report the caller's watermark. A restart creates a
new task and resets that task's history. These readings include instrumentation
overhead and do not establish requirements for unexercised paths.

The shared platform helper looks up uniquely named tasks without retaining
handles or allocating memory. On single-core firmware it suspends scheduling
during lookup and watermark scanning to prevent concurrent task deletion, then
resumes before logging. It is a no-op on host builds and multicore firmware,
where scheduler suspension alone cannot protect another core's task lifetime.
No extra task, timer, tracing buffer, or vendor modification is introduced.

For the next device capture, leave Local Server enabled and exercise measurement
GETs, configuration GET/PUTs, and repeated mDNS discovery from another device.
Include concurrent Cloud activity, at least one BMS timer tick after requests,
and Local Server disable/re-enable. Compare each task's lowest `stack_min` and
the heap snapshots across those operations. Heap deltas are system-wide and
can include concurrent allocations; this does not attribute every allocation
or transient heap peak to HTTP or mDNS. Device measurements remain user-only.

Verification: Go and reference firmware builds passed, all 1,761 host tests
passed, and Markdown lint and whitespace checks passed. The actual named task
watermarks were subsequently captured below; teardown/restart behavior remains
to be verified on hardware.

### Experiment 5: Local HTTP and mDNS Stack Reduction

The user authorized trying both smaller stacks together. Set the shared HTTP
driver stack to 4,096 bytes and Go's `CONFIG_MDNS_TASK_STACK_SIZE` to 2,048 bytes.
Keep the heap and stack diagnostics enabled. These changes should release
4,096 bytes while both services run. The shared HTTP constant also applies to
provisioning and the reference product; Go's mDNS change is product-specific.
Keep these as experiments without promoting settings into `sdkconfig.defaults`.

The baseline captures establish the following observed stack usage:

| Task / Operation | Reserved | Maximum Observed Use | Headroom at Trial Size |
|---|---:|---:|---:|
| HTTP configuration GET | 6,144 B | 2,212 B | 1,884 B |
| HTTP measurements GET | 6,144 B | 2,824 B | 1,272 B |
| mDNS | 4,096 B | 852 B | 1,196 B |

The user identified the first request in `result.local.log.1` as configuration
GET and the second as measurements GET. In `result.local.log.2`, configuration
PUTs and completed LED tests do not lower HTTP's 3,320-byte minimum untouched
stack. mDNS's minimum stays at 3,244 bytes across all three captures; explicit
discovery and GPS-test coverage are not established by those logs.

Heap exhaustion still occurs in the larger-stack baseline. The last capture
contains failed Cloud, WiFi, and network-task allocations, with a historical
minimum of 124 bytes already present at its start. Cloud's minimum untouched
stack is 2,976 bytes out of 6,144, or 3,168 bytes touched.

For this trial, compute observed usage as `4096 - stack_min` for HTTP and
`2048 - stack_min` for mDNS. Repeat GETs, configuration PUTs, actions, mDNS
discovery, and service restart alongside Cloud traffic. Include provisioning
before accepting the shared HTTP reduction. Check for successful Cloud POST
and GET, allocation failures, stack headroom, and the expected heap improvement.
The 4 KiB saving does not guarantee sufficient memory for the TLS peak.

Verification: Go and reference firmware builds passed, and Go's generated
configuration confirms the 2,048-byte mDNS stack. Markdown lint and whitespace
checks passed. This trial changes only a driver stack constant and Go's build
configuration; host tests were not rerun after changing the sizes.

#### Smaller-Stack Capture

`result.local.log.3` confirms HTTP at 4,096 bytes and mDNS at 2,048 bytes.
HTTP's minimum untouched stack is 1,268 bytes (2,828 bytes touched), and mDNS's
is 1,192 bytes (856 bytes touched). Requests, configuration PUTs, and LED tests
run without a recorded stack overflow. Disabling WiFi stops both services;
the following periodic diagnostic reports both tasks absent. Both services
start again after the subsequent button wake. This covers lifecycle behavior,
but does not establish provisioning, GPS-test, or explicit discovery coverage.

Two Cloud POSTs return HTTP 200, one in each boot period. After the second
boot's successful POST, minimum-ever free heap is only 732 bytes. The following
four POSTs fail with allocation failures; Cloud, WiFi, and the network task are
affected. The minimum eventually reaches 276 bytes. No Cloud configuration GET
appears in this capture. The stack reduction helps, but repeated TLS connections
remain unreliable. Retain these trial sizes and diagnostics as the progress
checkpoint; this is not acceptance of the overall memory budget.

### Verification Status and Remaining Decisions

- Combined WiFi IRAM experiment: build and device heap compared above; successful
  cloud operation and performance remain unverified. BLE count reduction is
  built and measured as described above, then restored to 24. The user's 8 KiB
  main stack confirms an 8 KiB heap gain with partial hardware-path coverage.
- Diagnostic Go and reference firmware builds: passed. Existing host suite:
  1,760 tests passed. The instrumented baseline capture has been analyzed above;
  broader hardware-path coverage remains pending.
- Instrumented Go binary: 1,943,696 bytes, up 2,080 bytes from the recorded
  baseline; 87,920 bytes remain in the smallest OTA slot. Diagnostics consume
  some flash and execution time; compare subsequent experiments with this
  instrumented baseline.
- WiFi code-placement heap saving: 13,568 bytes at matching boot checkpoints.
  BLE count reduction shows zero initialization saving; transfer peaks and
  performance remain unmeasured.
- Whether both pools can remain at 12 under OTA and concurrent WiFi: unverified.
- Three WiFi IRAM options: disabled in the user's local build; SLP remains on.
- IRAM text reduction, binary-size change, and runtime boot heap improvement
  measured. WiFi performance and reliability remain unverified.
- Cloud and Local Server: startup costs and stack capacities documented;
  baseline Cloud watermark captured; Local Server handler stack use and
  worst-case request allocation peaks remain unmeasured.
- Main task: stack reservation and function-frame lifetimes documented, with
  partial compiler frame sizes and baseline watermarks; a safe reduced stack
  capacity remains unverified.
- Whether WiFi buffers, filesystem caches, or service stacks need further tuning:
  decide after measurement; no replacement values have been selected.

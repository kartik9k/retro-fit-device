# Firmware Design Document

> **Owner: Firmware Agent.**
> This document is the single source of truth for firmware setup, architecture,
> and implementation decisions. Update it in the same commit as any code change
> that affects the content below.

---

## Project overview

ESP8266 firmware that reads distance from a DYP-A22 waterproof ultrasonic
sensor (I2C) and POSTs batched readings over Wi-Fi (or, in future, cellular)
to a companion FastAPI server. The ESP8266 RTOS SDK lives in `firmware/esp8266-rtos-sdk/` as a
submodule — no external SDK installation is required.

---

## 1. Project setup

### Prerequisites

- Linux or macOS host (Windows via WSL is untested)
- `make`, `python3`, `esptool.py` on PATH
- USB-serial adapter connected to ESP8266 UART0 (GPIO1/GPIO3 by default)
- No external SDK needed — the submodule is self-contained

### Build

```bash
cd firmware

# First-time: initialise the SDK submodule
git submodule update --init --recursive

# (Optional) configure serial port, flash size, etc.
make menuconfig

# Build only
make

# Flash to device (default port: /dev/ttyUSB0)
make flash

# Flash and open serial monitor immediately after
make flash monitor

# Open serial monitor without rebuilding
make monitor
```

Build artefacts land in `firmware/build/`. `firmware/sdkconfig` holds the
active configuration; `firmware/sdkconfig.defaults` seeds it on first run with:
- `CONFIG_HTTPD_MAX_REQ_HDR_LEN=1024` — accommodates iOS captive-portal headers
- `CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH=4096` — timer-daemon stack raised because `dyp_a22` timer callbacks perform I2C transactions
- `CONFIG_ESP_TLS_INSECURE` / `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY` — dev-only TLS; remove when cert pinning is implemented (DEFERRED §11)

### Key configuration constants (`main/app_main.c`)

These must be reviewed before flashing to a new deployment:

| Constant | Default | Description |
|----------|---------|-------------|
| `POST_URL` | Two options in `firmware/main/app_main.c` — uncomment the correct line | **Mode 1 (LAN):** `http://192.168.1.65:5000/api/data` — find LAN IP with `ip route get 1 \| awk '{print $7; exit}'`. **Mode 3 (Azure):** `https://retro-fit-server.nicepebble-7757b674.uksouth.azurecontainerapps.io/api/data`. TLS cert (ISRG Root X1) is embedded in `wifi_transport.c` — no extra config needed. |
| `POST_PERIOD_US` | 30 000 000 µs (30 s) | How often the batch POST fires |
| `SENSOR_PERIOD_MS` | 2 000 ms | Sensor sampling interval (application sleep between `read_mm()` calls) |
| `READING_QUEUE_DEPTH` | 30 | Max readings buffered between POSTs (~60 s at 2 s/sample) |

---

## 2. NVS storage

### Partition

| Field | Value |
|-------|-------|
| Partition label | `nvs` (SDK default) |
| Initialisation | `nvs_flash_init()` in `wifi_manager_init()` — called once at boot |
| Erase on error | Partition is erased and reinitialised on `ESP_ERR_NVS_NO_FREE_PAGES` or `ESP_ERR_NVS_NEW_VERSION_FOUND` — this clears **all** namespaces |

> **Pending (DEFERRED §4):** When NVS-based runtime transport selection is
> added, `nvs_flash_init()` must move from `wifi_manager_init()` to
> `app_main()` so `transport_get()` can read its key before the transport
> driver initialises. Update this section at that time.

### Namespaces and keys

#### `wifi_mgr` — managed by `firmware/main/wifi_manager.c`

| Key | Type | Max size | Default (absent) | Description |
|-----|------|----------|------------------|-------------|
| `ssid` | `NVS_TYPE_STR` | 33 bytes | *(absent)* | Wi-Fi SSID. Absent on a virgin device — triggers provisioning mode. |
| `pass` | `NVS_TYPE_STR` | 65 bytes | `""` | Wi-Fi password. May be empty for open networks. |

**Write path:** captive-portal HTTP POST handler (`handle_post`) during provisioning.  
**Read path:** `nvs_load()` in `wifi_manager_init()` on every normal boot.  
**Factory reset:** erasing NVS removes these keys; the device re-enters provisioning on next boot.

#### `transport` — *planned, not yet implemented* (see DEFERRED §4)

> Managed by `firmware/main/transport.c` once implemented.

| Key | Type | Max size | Default (absent) | Description |
|-----|------|----------|------------------|-------------|
| `type` | `NVS_TYPE_STR` | 10 bytes | `"wifi"` | Active transport: `"wifi"` or `"cellular"`. Read once in `transport_get()` at boot. |

**Write path:** not written by firmware — set via factory flash tool or OTA config.

### NVS key size constants

Defined in `firmware/main/wifi_manager.c`:

| Constant | Value | Purpose |
|----------|-------|---------|
| `WM_SSID_LEN` | 33 | Read buffer for SSID |
| `WM_PASS_LEN` | 65 | Read buffer for password |

### NVS change log

| Date | Change | Author |
|------|--------|--------|
| 2026-04-20 | Initial layout: `wifi_mgr/ssid`, `wifi_mgr/pass` | Firmware Agent |

---

## 3. Architecture

### Task layout

| Task | Priority | Stack | Purpose |
|------|----------|-------|---------|
| `sensor_task` | **8** | 2 048 B | Calls `read_mm()` every 2 s; blocks on `s_result_sem` (not `vTaskDelay`) so the 80 ms measurement window does not stall the task |
| `post_task` | **5** | 4 096 B | Blocks on task notification; drains the reading queue and fires one batch POST |
| `dns_server_task` | **5** | 2 048 B | UDP DNS server — active only during provisioning (captive portal); not started on normal boot |
| FreeRTOS timer-daemon | SDK | 4 096 B | Runs `meas_timer_cb` (I2C result read + next trigger) and `health_scan_cb` (periodic I2C bus scan); stack raised in `sdkconfig.defaults` because callbacks perform I2C transactions |
| Wi-Fi stack tasks | ~23 | SDK-managed | Managed entirely by the ESP8266 RTOS SDK |

**Priority rationale:**
- `sensor_task` at 8 is retained from HC-SR04; safe to lower once the DYP-A22
  pipeline is validated on the bench (I2C has no busy-wait — see sensor section below).
- `post_task` at 5 ensures blocking HTTP (which can stall for seconds on a
  slow or unreachable server) never starves the sensor.
- `post_timer_cb` runs in the ESP timer task — it only calls
  `xTaskNotifyGive()` and never blocks. Any blocking operation in a timer
  callback will deadlock.

### Transport abstraction (`firmware/main/transport.h`)

All network drivers implement `transport_driver_t` — three function pointers:

| Function | Blocking? | Description |
|----------|-----------|-------------|
| `init()` | Yes | One-time hardware init; blocks until connected and IP is held |
| `is_ready()` | No | Called from timer context; returns true when ready to POST |
| `post(url, body, len)` | Yes | Sends an HTTP POST with a pre-built JSON body |

`transport_get()` in `firmware/main/transport.c` returns the active driver. Currently
always returns `&wifi_transport`; NVS-based runtime selection is deferred
(see DEFERRED §4).

**Drivers:**
- `firmware/main/wifi_transport.c` — wraps `wifi_manager_init()` and `esp_http_client`
- `firmware/main/cellular_transport.c` — stub returning `ESP_ERR_NOT_SUPPORTED`; real
  SIM7080G AT-command driver slots in here (see DEFERRED §1)

### Sensor abstraction (`firmware/main/distance_sensor.h`)

All sensor drivers implement `distance_sensor_t` — two function pointers:

| Function | Description |
|----------|-------------|
| `init()` | One-time hardware initialisation |
| `read_mm()` | Trigger a measurement; returns mm or `DISTANCE_SENSOR_ERR` (`INT32_MIN`) on timeout/out-of-range |

`SENSOR_TYPE_TAG` (also in `distance_sensor.h`) is a short string included in
every batch POST under the `sensor` field. Currently `"us"` for ultrasonic —
both current drivers share this tag since the measurement principle is the same.

### Sensor configurations (hardware v1 and v2)

Two sensor configurations are maintained. See `HARDWARE.md` § Hardware
configurations for the full wiring and board-change details.

| | **v1 — HC-SR04** | **v2 — DYP-A22** |
|---|---|---|
| Status | Inactive — removed from board | **Active** |
| Driver | `firmware/main/hcsr04.c` | `firmware/main/dyp_a22.c` |
| Interface | TRIG/ECHO pulse on GPIO5/GPIO4 | I2C on GPIO4 (SDA) / GPIO5 (SCL) |

#### DYP-A22 I2C protocol

Address `0x74`, 100 kHz. Measurement sequence:

1. **Trigger** — write register `0x10`, data `0xBD`
2. **Wait** — 80 ms minimum (datasheet); reading early returns `0xFFFF`
3. **Read** — write register `0x02`, repeated-start, read 2 bytes `[H, L]`
4. **Discard** — `0x0000` (blind zone / out of range) and `0xFFFF` (not ready) both map to `DISTANCE_SENSOR_ERR`

#### DYP-A22 driver architecture

The driver is pipeline-based and non-blocking. All I2C access is serialised by
`s_bus_mutex`; all callers take and release it around each transaction.

```
init()
  ├─ i2c_scan() under mutex        ← initial bus survey
  ├─ xTimerCreate(dyp_health, 1 h) ← periodic health scan
  └─ dyp_trigger()                 ← primes the pipeline

dyp_trigger() [sensor_task or timer-daemon]
  ├─ take s_bus_mutex
  ├─ I2C write: reg 0x10, data 0xBD  (~1 ms)
  ├─ give s_bus_mutex
  └─ xTimerStart(s_meas_timer, 80 ms)

meas_timer_cb() [timer-daemon, 80 ms after trigger]
  ├─ take s_bus_mutex
  ├─ I2C read: reg 0x02 → [H, L]    (~1 ms)
  ├─ give s_bus_mutex
  ├─ store result in s_last_mm
  ├─ xSemaphoreGive(s_result_sem)   ← wakes sensor_task
  └─ dyp_trigger()                  ← keeps pipeline self-sustaining

health_scan_cb() [timer-daemon, every DYP_HEALTH_SCAN_MS]
  ├─ try take s_bus_mutex (skip if busy)
  ├─ i2c_scan()
  └─ give s_bus_mutex

dyp_a22_read_mm() [sensor_task]
  ├─ xSemaphoreTake(s_result_sem, 500 ms)
  │    └─ on timeout: restarts pipeline via dyp_trigger(), returns ERR
  └─ return s_last_mm
```

**Key properties:**
- `sensor_task` never calls `vTaskDelay` for the measurement window; it blocks
  on a semaphore, so the CPU is free to run other tasks during the 80 ms.
- The pipeline runs continuously at ~82 ms/cycle; `read_mm()` always returns
  immediately because `s_result_sem` is already given by the time sensor_task
  wakes from its 2-second application sleep.
- `health_scan_cb` and `meas_timer_cb` share the timer-daemon task and execute
  sequentially — no re-entrancy issues between them.
- To change the health scan interval, edit `DYP_HEALTH_SCAN_MS` in `dyp_a22.c`.

#### Switching sensor configuration

Edit the two marked lines in `firmware/main/app_main.c`:

```c
/* ---- active sensor: swap this include + pointer to change hardware ---- */
// v1 — HC-SR04 (inactive):
// #include "hcsr04.h"
// static const distance_sensor_t *s_sensor = &hcsr04_sensor;

// v2 — DYP-A22 (active):
#include "dyp_a22.h"
static const distance_sensor_t *s_sensor = &dyp_a22_sensor;
/* ----------------------------------------------------------------------- */
```

No other files need to change. The `sensor_task` priority rationale differs
between drivers:
- **v1 (HC-SR04):** `sensor_task` at priority 8 is critical — the TRIG/ECHO
  busy-wait must not be preempted mid-pulse.
- **v2 (DYP-A22, active):** I2C has no busy-wait; priority 8 is safe to lower
  once the driver is validated on the bench. Notify Hardware Agent before
  changing priority.

### Wi-Fi provisioning flow (`firmware/main/wifi_manager.c`)

**First boot** (no NVS credentials):
1. Starts SoftAP named `retro-fit-XXXXXX` (last 3 MAC bytes, open network)
2. Starts a UDP DNS server on port 53 that redirects all queries to the AP IP
   (`192.168.4.1`) — this triggers OS captive-portal popups automatically
3. Serves a credential form at `http://192.168.4.1`
4. On submit: saves SSID + password to NVS namespace `wifi_mgr`, then calls
   `esp_restart()` — never returns

**Normal boot** (credentials present):
1. Reads `ssid` and `pass` from NVS namespace `wifi_mgr`
2. Connects in STA mode; blocks until IP is obtained
3. Reconnects automatically on disconnection (handled in `wifi_event_handler`)

### Concurrency and shared state

The ESP8266 is single-core. 32-bit aligned reads and writes are atomic — no
mutex is needed for `int32_t` or `uint32_t` shared between tasks, provided
only one task writes and the other only reads. Where multiple tasks perform
I2C transactions (as in the DYP-A22 driver), a FreeRTOS mutex serialises bus access.

Current shared variables:

| Variable | Type | Writer | Reader | Safe? |
|----------|------|--------|--------|-------|
| `s_reading_queue` | `QueueHandle_t` | `sensor_task` (send) | `post_task` (receive) | Yes — FreeRTOS queue is thread-safe |
| `s_transport` | `const transport_driver_t *` | Set once in `app_main` before tasks start | All tasks | Yes — written before any reader exists |
| `s_last_mm` (in `dyp_a22.c`) | `volatile int32_t` | timer-daemon (`meas_timer_cb`) | `sensor_task` (`read_mm`) | Yes — written before `xSemaphoreGive(s_result_sem)`; semaphore acts as a happens-before barrier |
| `s_bus_mutex` (in `dyp_a22.c`) | `SemaphoreHandle_t` | — | `sensor_task`, timer-daemon | Yes — FreeRTOS mutex; all I2C callers take/release it around each transaction |

### Batch POST format

JSON body built in `http_post_batch()` (`main/app_main.c`). Buffer size
`BATCH_BUF_SZ = 900` bytes (worst-case: 30 readings × 27 chars + 50-char
envelope = ~860 bytes). A truncation warning is logged if the buffer fills.

Wire format is defined in `API_CONTRACT.md` — do not change it here without
following the Firmware ↔ Server notification rule.

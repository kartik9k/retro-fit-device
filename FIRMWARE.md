# Firmware Design Document

> **Owner: Firmware Agent.**
> This document is the single source of truth for firmware setup, architecture,
> and implementation decisions. Update it in the same commit as any code change
> that affects the content below.

---

## Project overview

ESP8266 firmware that reads distance from an HC-SR04 ultrasonic sensor and
POSTs batched readings over Wi-Fi (or, in future, cellular) to a companion
FastAPI server. The ESP8266 RTOS SDK lives in `firmware/esp8266-rtos-sdk/` as a
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
active configuration; `firmware/sdkconfig.defaults` seeds it on first run
(currently only raises `CONFIG_HTTPD_MAX_REQ_HDR_LEN` to 1024 to accommodate
iOS captive-portal headers).

### Key configuration constants (`main/app_main.c`)

These must be reviewed before flashing to a new deployment:

| Constant | Default | Description |
|----------|---------|-------------|
| `POST_URL` | `http://192.168.1.65:5000/api/data` | **Must match the server's LAN IP.** Find it with: `ip route get 1 \| awk '{print $7; exit}'`. Set in `firmware/main/app_main.c`. |
| `POST_PERIOD_US` | 30 000 000 µs (30 s) | How often the batch POST fires |
| `SENSOR_PERIOD_MS` | 2 000 ms | HC-SR04 sampling interval |
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
| `sensor_task` | **8** | 2 048 B | Calls `read_mm()` in a loop; busy-waits on GPIO for the HC-SR04 echo pulse |
| `post_task` | **5** | 4 096 B | Blocks on task notification; drains the reading queue and fires one batch POST |
| `dns_server_task` | **5** | 2 048 B | UDP DNS server — active only during provisioning (captive portal); not started on normal boot |
| Wi-Fi stack tasks | ~23 | SDK-managed | Managed entirely by the ESP8266 RTOS SDK |

**Priority rationale:**
- `sensor_task` at 8 ensures the HC-SR04 echo pulse busy-wait is never
  preempted mid-measurement. Preemption mid-pulse produces a wrong distance.
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

To swap sensors: change the `#include` and the `s_sensor` pointer in
`app_main.c`. Also update `SENSOR_TYPE_TAG` in `distance_sensor.h` to match
the new sensor's type string — this tag is included in every batch POST.

**Current driver:** `firmware/main/hcsr04.c` — HC-SR04 ultrasonic, `SENSOR_TYPE_TAG = "us"`

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
only one task writes and the other only reads.

Current shared variables:

| Variable | Type | Writer | Reader | Safe? |
|----------|------|--------|--------|-------|
| `s_reading_queue` | `QueueHandle_t` | `sensor_task` (send) | `post_task` (receive) | Yes — FreeRTOS queue is thread-safe |
| `s_transport` | `const transport_driver_t *` | Set once in `app_main` before tasks start | All tasks | Yes — written before any reader exists |

### Batch POST format

JSON body built in `http_post_batch()` (`main/app_main.c`). Buffer size
`BATCH_BUF_SZ = 900` bytes (worst-case: 30 readings × 27 chars + 50-char
envelope = ~860 bytes). A truncation warning is logged if the buffer fills.

Wire format is defined in `API_CONTRACT.md` — do not change it here without
following the Firmware ↔ Server notification rule.

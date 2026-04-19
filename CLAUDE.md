# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Team — agent roles and discussion protocol

Three specialist agents own this project. Route every task to the right owner. For anything that crosses boundaries, run a multi-agent discussion first — collect all perspectives, resolve conflicts, then implement.

| Agent | Owns | Key constraints |
|-------|------|-----------------|
| **Firmware** | `main/` — FreeRTOS tasks, ESP8266 SDK, HTTP client, NVS, build system | Stack/heap limits, single-core atomicity, timer-context rules |
| **Hardware** | GPIO wiring, voltage levels, sensor selection, circuit design | 3.3 V / 5 V interfacing, HC-SR04 timing, drive strength |
| **Server** | `server/` — FastAPI, SQLite schema, Pydantic models, SSE, deployment | Wire format compatibility, API contract, async correctness |

**Discussion protocol for cross-boundary changes:**
1. Spawn all affected agents with their role context and the problem.
2. Collect perspectives; surface conflicts (e.g. firmware memory limit vs. server schema preference).
3. Synthesise a decision that satisfies all constraints.
4. Implement — breaking wire-contract changes go in **one atomic commit** across all affected files.

## What this project is

An ESP8266 firmware that reads distance from an HC-SR04 ultrasonic sensor and POSTs readings over Wi-Fi to a companion Python/FastAPI server. The repo is self-contained: the ESP8266 RTOS SDK lives in `esp8266-rtos-sdk/` as a submodule, so no external SDK installation is required.

## Build & flash (firmware)

The Makefile automatically exports `IDF_PATH` to the bundled SDK, so no environment setup is needed before `make`.

```bash
# Configure (select serial port, flash size, etc.)
make menuconfig

# Build
make

# Flash + monitor (default port /dev/ttyUSB0)
make flash monitor

# Build + flash + monitor in one step
make flash monitor

# Monitor only (no rebuild)
make monitor
```

The build output lands in `build/`. `sdkconfig` holds the active config; `sdkconfig.defaults` seeds it (currently only raises `CONFIG_HTTPD_MAX_REQ_HDR_LEN` to 1024 for iOS captive-portal headers).

## Run the server

```bash
cd server
pip install -r requirements.txt
uvicorn main:app --host 0.0.0.0 --port 5000 --reload
```

The server binds to `0.0.0.0` so the ESP8266 can reach it over LAN. Find the host machine's LAN IP with:
```bash
ip route get 1 | awk '{print $7; exit}'
```
Then set `POST_URL` in `main/app_main.c` to `http://<LAN_IP>:5000/api/data`.

## Firmware architecture (`main/`)

### Task layout

| Task | Priority | Purpose |
|------|----------|---------|
| `sensor_task` | 8 | Calls `read_mm()` in a loop; busy-waits on GPIO for the echo pulse |
| `post_task` | 5 | Blocks on `s_post_queue`; owns the blocking `esp_http_client_perform()` call |
| `dns_server_task` | 5 | UDP DNS server active only during provisioning (captive portal) |
| Wi-Fi stack tasks | ~23 | Managed by the SDK |

`sensor_task` runs at priority 8 so the HC-SR04 echo pulse busy-wait is never preempted mid-measurement. `post_task` is deliberately lower so HTTP (which can block for seconds) never starves the sensor. The timer callback (`post_timer_cb`) only enqueues a snapshot — it never blocks.

### Sensor abstraction (`distance_sensor.h`)

All sensor drivers implement `distance_sensor_t` — two function pointers (`init`, `read_mm`). To swap sensors, change the `#include` and pointer in `app_main.c`. `read_mm` returns millimetres or `DISTANCE_SENSOR_ERR` (`INT32_MIN`) on timeout/out-of-range.

### Wi-Fi / provisioning (`wifi_manager.c`)

- **First boot** (no NVS credentials): starts SoftAP `retro-fit-XXXXXX`, runs a DNS server that redirects all queries to the AP IP so OS captive-portal popups appear automatically, and serves a credential form at `http://192.168.4.1`. On submit the device saves SSID/password to NVS and restarts.
- **Normal boot**: reads credentials from NVS namespace `wifi_mgr`, connects in STA mode, blocks until IP is obtained, then reconnects automatically on drop.

### Shared state

`s_distance_mm` (volatile `int32_t`) is written by `sensor_task` and read by the timer callback. This is safe without a mutex because the ESP8266 is single-core and 32-bit writes are atomic.

## Server architecture (`server/`)

FastAPI + SQLite. Three main endpoints:
- `POST /api/data` — ingest a reading, persist to SQLite, fan-out to SSE subscribers
- `GET /api/data` — query recent readings (optional `device` filter, `limit` param)
- `GET /api/stream` — SSE stream; one `asyncio.Queue` per connected client, keyed by device name

The live visualizer UI is served from `server/static/index.html` and subscribes to `/api/stream`.

## Key configuration constants (`main/app_main.c`)

| Constant | Default | Description |
|----------|---------|-------------|
| `POST_URL` | `http://192.168.1.100:5000/api/data` | **Must be updated** to actual server LAN IP |
| `POST_PERIOD_US` | 30 s | How often readings are POSTed |
| `SENSOR_PERIOD_MS` | 2000 ms | HC-SR04 sampling interval |

## HC-SR04 wiring

| Sensor pin | ESP8266 pin | Notes |
|------------|-------------|-------|
| VCC | 5 V (Vin) | |
| GND | GND | |
| TRIG | GPIO5 | Direct 3.3 V drive is sufficient |
| ECHO | GPIO4 | Via 1 kΩ / 2 kΩ voltage divider — sensor outputs 5 V |

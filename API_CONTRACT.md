# Firmware ↔ Server API Contract

> **Joint ownership: Firmware Agent and Server Agent.**
> This document is the single source of truth for the wire protocol between the ESP8266 firmware and the Python server.
>
> **Change protocol:**
> - No change to this document may be made unilaterally. Both agents must agree before anything is merged.
> - Either agent may raise a proposed change; the other must explicitly acknowledge it.
> - Any agreed change must land in **one atomic commit** that updates this document, the firmware, and the server together — the codebase must never be in a state where the two sides are incompatible.

---

## Endpoint

| Field | Value |
|-------|-------|
| Method | `POST` |
| Path | `/api/data` |
| Base URL — Mode 1 (LAN) | `http://<server-lan-ip>:5000` — plain HTTP, device and server on same Wi-Fi |
| Base URL — Mode 3 (Azure) | `https://retro-fit-server.nicepebble-7757b674.uksouth.azurecontainerapps.io` |
| Content-Type | `application/json` |
| Auth | None |
| TLS | Required for Mode 3. Firmware embeds ISRG Root X1 CA cert in `wifi_transport.c`. Replace cert if server moves away from Let's Encrypt. |

---

## Request body

```json
{
  "proto": 1,
  "device": "retro-fit",
  "sensor": "us",
  "readings": [
    { "v": 12.3,  "t": 360000 },
    { "v": null,  "t": 362000 },
    { "v": 12.5,  "t": 364000 }
  ],
  "events": [
    { "k": "i2c_health",    "v": true,       "t": 360000 },
    { "k": "rssi",          "v": -67,         "t": 360000 },
    { "k": "reboot_reason", "v": "power_on",  "t": 0      }
  ]
}
```

### Fields

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `proto` | `integer` | Yes | Protocol major version. Current value: `1`. Server rejects with `400` if the value exceeds the highest version it supports. Absent in pre-versioning firmware — server must treat absence as `1`. Defined in firmware as `PROTO_VERSION` in `app_main.c`. |
| `device` | `string` | Yes | Non-empty device identifier. Fixed value `"retro-fit"` in current firmware. |
| `sensor` | `string` | Yes | Short sensor-type tag. Current values: `"us"` (DYP-A22 ultrasonic). New sensor modules add a new tag here — no other field changes required. Defined in firmware as `SENSOR_TYPE_TAG` in `distance_sensor.h`. |
| `readings` | `array` | Yes | Ordered list of samples, oldest first. May be empty (server returns 0 inserts). |
| `readings[].v` | `number \| null` | Yes | Sensor value in the unit implied by `sensor` (cm for `"us"`). `null` when sensor returned out-of-range or timeout. |
| `readings[].t` | `integer` | Yes | Milliseconds since device boot (`esp_timer_get_time() / 1000`). Monotonically increasing within a batch. Wraps at ~49.7 days. |
| `events` | `array` | No | Device telemetry and health events. Omitted entirely when there are no events to report. Order is not significant. |
| `events[].k` | `string` | Yes | Event key. Must be a value from the key registry below. |
| `events[].v` | `bool \| number \| string \| null` | Yes | Event value. Type is determined by the key — see registry. |
| `events[].t` | `integer` | Yes | Same semantics as `readings[].t` — ms since device boot. |

### Firmware encoding notes

- `readings[].v` is formatted as `%d.%d` using integer mm arithmetic: `mm/10` . `mm%10`. This produces one decimal digit (e.g. `12.3`, `0.5`, `650.0`). The server must treat it as a float, not a fixed-precision decimal.
- `t` is cast from `int64_t` to `uint32_t`. Relative differences within a single batch are always < 30 s so wrap-around never affects delta computation.
- Batch size: up to `READING_QUEUE_DEPTH` (currently 30) readings per POST. The firmware caps the JSON body at `BATCH_BUF_SZ` (currently 900 bytes) and logs a warning if truncated.
- POST period: every `POST_PERIOD_US` (currently 30 s).
- **When to send each event is a firmware implementation detail and is not part of this contract.** The server must handle any key appearing zero or more times per POST without error.

---

## Event key registry

Both sides must agree before adding a new key. Adding a key requires a change-log entry here; no other contract fields change.

| Key | Value type | Description |
|-----|------------|-------------|
| `i2c_health` | `bool` | `true` if the DYP-A22 was visible on the I2C bus at the last health scan; `false` if absent. |
| `rssi` | `integer` | Wi-Fi received signal strength in dBm. Typical range −30 (excellent) to −90 (poor). |
| `heap_free` | `integer` | Free heap memory in bytes at time of POST (`esp_get_free_heap_size()`). |
| `reboot_reason` | `string` | Why the device last rebooted. Sent once on the first successful POST after boot, then not again until the next reboot. Values: `"power_on"`, `"sw_reset"`, `"watchdog"`, `"deep_sleep_wake"`, `"unknown"`. |
| `post_failures` | `integer` | Count of consecutive failed POST attempts since last successful POST. Omitted (not sent) when zero. |

---

## Response

### 201 Created (success)

```json
{ "inserted": 3, "ids": [42, 43, 44] }
```

| Field | Type | Description |
|-------|------|-------------|
| `inserted` | `integer` | Number of rows written to the database. |
| `ids` | `array[integer]` | Auto-increment IDs of the inserted rows, in the same order as `readings`. |

### Error responses

| Status | Meaning |
|--------|---------|
| 422 | Malformed body — missing required field or wrong type. Body: `{"detail": [...]}` |
| 500 | Server-side failure. Firmware logs `POST failed` and retries on the next timer tick. |

---

## Server timestamp reconstruction

The server does not receive a wall-clock time from the firmware. It reconstructs per-reading times as:

```
recorded_at[i] = now - timedelta(milliseconds = (latest_timestamp_ms - readings[i].timestamp_ms))
```

Where `now` is the UTC time the POST was received and `latest_timestamp_ms` is the maximum `timestamp_ms` in the batch. The most-recent reading therefore gets `recorded_at = now`; earlier readings are backdated by their relative offset.

---

## SSE event format (`GET /api/stream`)

Each inserted reading is published individually to SSE subscribers as:

```
data: {"id":42,"device":"retro-fit","sensor_type":"us","value":12.3,"recorded_at":"2026-04-20T10:00:00.123456+00:00"}
```

The SSE payload is a single JSON object matching `ReadingOut`. This is not negotiated with the firmware (firmware has no SSE client), but any change to the SSE schema that affects the visualiser UI should still be flagged to the team.

---

## Change log

| Date | Change | Raised by | Agreed by |
|------|--------|-----------|-----------|
| 2026-04-20 | Initial contract documented — batch JSON format with `timestamp_ms` | Firmware Agent | Server Agent |
| 2026-04-20 | Payload optimisation: `distance_cm`→`v`, `timestamp_ms`→`t`, add `sensor` field; `BATCH_BUF_SZ` 1600→900; DB columns `distance_cm`→`value`, add `sensor_type` | Server Agent | Firmware Agent |
| 2026-04-21 | Add Mode 3 Azure HTTPS base URL; TLS via ISRG Root X1 CA cert embedded in firmware; Mode 1 LAN HTTP unchanged | Firmware Agent | Server Agent |
| 2026-07-13 | Add optional `events` array for device telemetry; define event key registry (`i2c_health`, `rssi`, `heap_free`, `reboot_reason`, `post_failures`); send frequency is firmware implementation detail and not part of contract | Firmware Agent | Server Agent |
| 2026-07-13 | Add `proto` integer field (major protocol version); current value `1`; server treats absence as `1` for backward compat; rejects unsupported versions with `400` | Firmware Agent | Server Agent |

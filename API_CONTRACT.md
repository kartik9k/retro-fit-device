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
| Base URL (configurable) | `http://<server-lan-ip>:5000` |
| Content-Type | `application/json` |
| Auth | None |

---

## Request body

```json
{
  "device": "retro-fit",
  "sensor": "us",
  "readings": [
    { "v": 12.3,  "t": 360000 },
    { "v": null,  "t": 362000 },
    { "v": 12.5,  "t": 364000 }
  ]
}
```

### Fields

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `device` | `string` | Yes | Non-empty device identifier. Fixed value `"retro-fit"` in current firmware. |
| `sensor` | `string` | Yes | Short sensor-type tag. Current values: `"us"` (HC-SR04 ultrasonic). New sensor modules add a new tag here — no other field changes required. Defined in firmware as `SENSOR_TYPE_TAG` in `distance_sensor.h`. |
| `readings` | `array` | Yes | Ordered list of samples, oldest first. May be empty (server returns 0 inserts). |
| `readings[].v` | `number \| null` | Yes | Sensor value in the unit implied by `sensor` (cm for `"us"`). `null` when sensor returned out-of-range or timeout. |
| `readings[].t` | `integer` | Yes | Milliseconds since device boot (`esp_timer_get_time() / 1000`). Monotonically increasing within a batch. Wraps at ~49.7 days. |

### Firmware encoding notes

- `v` is formatted as `%d.%d` using integer mm arithmetic: `mm/10` . `mm%10`. This produces one decimal digit (e.g. `12.3`, `0.5`, `650.0`). The server must treat it as a float, not a fixed-precision decimal.
- `t` is cast from `int64_t` to `uint32_t`. Relative differences within a single batch are always < 30 s so wrap-around never affects delta computation.
- Batch size: up to `READING_QUEUE_DEPTH` (currently 30) readings per POST. The firmware caps the JSON body at `BATCH_BUF_SZ` (currently 900 bytes) and logs a warning if truncated.
- POST period: every `POST_PERIOD_US` (currently 30 s).

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

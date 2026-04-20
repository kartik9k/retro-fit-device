# Server Design Document

> **Owner: Server Agent.**
> This document is the single source of truth for server setup, architecture,
> and database design. Update it in the same commit as any code change that
> affects the content below.

---

## Project overview

Python/FastAPI server that receives batched sensor readings from ESP8266
devices, persists them to SQLite, and streams live updates to browser clients
over Server-Sent Events (SSE). A static visualiser UI is bundled in
`server/static/`.

---

## 1. Setup

### Prerequisites

- Python 3.10+
- The device must be able to reach this host over the network (LAN or cellular)

### Install and run

```bash
cd server
pip install -r requirements.txt
uvicorn main:app --host 0.0.0.0 --port 5000 --reload
```

The server binds to `0.0.0.0` so devices on the LAN (or behind NAT on
cellular) can POST to it. `--reload` enables auto-restart on code changes;
omit it in production.

### Find the LAN IP (for firmware configuration)

```bash
ip route get 1 | awk '{print $7; exit}'
```

Set the result as `POST_URL` in `main/app_main.c`:
```
http://<LAN_IP>:5000/api/data
```

### Database

SQLite database is created automatically at `server/readings.db` on first
startup. The file is git-ignored. Schema migrations run automatically in
`init_db()` at startup — no manual steps required.

---

## 2. Server design

### Endpoints

| Method | Path | Description |
|--------|------|-------------|
| `POST` | `/api/data` | Ingest a batch of readings from a device; persist to SQLite; fan-out each reading to SSE subscribers |
| `GET` | `/api/data` | Query recent readings. Params: `device` (filter), `limit` (1–1000, default 100) |
| `GET` | `/api/stream` | SSE stream. Param: `device` (default `"retro-fit"`). One `asyncio.Queue` per connected client |
| `GET` | `/health` | Liveness check — returns `{"status": "ok"}` |
| `GET` | `/` | Serves the live visualiser UI (`server/static/index.html`) |

### Async model

FastAPI runs on an async event loop (uvicorn + asyncio). The POST and GET
data endpoints are **synchronous** (`def`, not `async def`) because SQLite
access via the stdlib `sqlite3` module is blocking. FastAPI runs these in a
thread-pool automatically.

The SSE endpoint (`/api/stream`) is `async def` and uses `asyncio.Queue` to
receive events published by the synchronous POST handler via `queue.put_nowait()`.

### SSE fan-out

`_sse_subscribers` maps `device → set[asyncio.Queue]`. When a POST lands:
1. Each inserted row is passed to `_publish(device, row)`
2. `_publish` iterates the subscriber set and calls `put_nowait` on each queue
3. If a queue is full (`maxsize=128`) the event is dropped — slow clients are
   skipped rather than allowed to block the POST handler

### Pydantic models

| Model | Direction | Fields |
|-------|-----------|--------|
| `SingleReading` | Inbound (per reading) | `v: float\|null`, `t: int` |
| `BatchReading` | Inbound (full POST body) | `device: str`, `sensor: str`, `readings: list[SingleReading]` |
| `ReadingOut` | Outbound (GET + SSE) | `id`, `device`, `sensor_type`, `value`, `recorded_at` |

Wire format is defined in `API_CONTRACT.md` — do not change models here
without following the Firmware ↔ Server notification rule.

### Timestamp reconstruction

The device sends `t` (ms since boot), not a wall-clock time. The server
reconstructs per-reading wall-clock times as:

```
recorded_at[i] = now - (max(t) - t[i]) ms
```

The most-recent reading in a batch gets `recorded_at = now`; earlier readings
are backdated by their relative offset.

---

## 3. Database design

### Engine

SQLite via Python stdlib `sqlite3`. Single file at `server/readings.db`.
Connection-per-request pattern via the `get_db()` context manager — commits
on success, rolls back on exception, closes on exit.

### Schema

```sql
CREATE TABLE readings (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    device      TEXT    NOT NULL,
    sensor_type TEXT    NOT NULL DEFAULT 'us',
    value       REAL,
    recorded_at TEXT    NOT NULL
);
```

| Column | Type | Description |
|--------|------|-------------|
| `id` | INTEGER PK | Auto-increment row ID; returned to firmware in POST response |
| `device` | TEXT | Device identifier string (e.g. `"retro-fit"`) |
| `sensor_type` | TEXT | Short sensor tag from the `sensor` field in the batch POST (e.g. `"us"`) |
| `value` | REAL | Sensor reading in the unit implied by `sensor_type`; `NULL` on error/timeout |
| `recorded_at` | TEXT | ISO 8601 UTC timestamp (reconstructed from device boot-relative offset) |

### Migration strategy

`init_db()` runs at every startup. It:
1. Creates the table if it does not exist (fresh install)
2. Checks `PRAGMA table_info(readings)` for the `distance_cm` column — if
   present, the old schema is detected and the table is recreated via a
   `CREATE/INSERT/DROP/RENAME` sequence, backfilling `sensor_type = 'us'`

Future migrations follow the same pattern: detect the old schema by checking
for a sentinel column or missing column, then recreate. SQLite's limited
`ALTER TABLE` support (no `DROP COLUMN` before 3.35, no type changes) makes
recreate-and-copy the standard approach.

### Schema change log

| Date | Change | Author |
|------|--------|--------|
| 2026-04-20 | Initial schema: `id`, `device`, `distance_cm`, `recorded_at` | Server Agent |
| 2026-04-20 | `distance_cm` → `value`, add `sensor_type`; migration added to `init_db()` | Server Agent |

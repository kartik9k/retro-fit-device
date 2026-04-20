# Server Design Document

> **Owner: Server Agent.**
> This document is the single source of truth for server setup, architecture,
> and database design. Update it in the same commit as any code change that
> affects the content below.

---

## Project overview

Python/FastAPI server that receives batched sensor readings from ESP8266
devices, persists them to PostgreSQL, and streams live updates to browser
clients over Server-Sent Events (SSE). A static visualiser UI is bundled in
`server/static/`.

---

## 1. Setup

### Prerequisites

- Docker + Docker Compose (recommended — runs server + PostgreSQL together)
- **or** Python 3.11+ and a local/remote PostgreSQL 16 instance

### Option A — Docker Compose (recommended for local development)

Starts PostgreSQL and the server together. Server is accessible at `localhost:5000`.

```bash
docker compose up --build
```

Tear down (keeps database volume):
```bash
docker compose down
```

Tear down including data:
```bash
docker compose down -v
```

### Option B — run server directly (requires local PostgreSQL)

```bash
cd server
cp .env.example .env          # then edit DATABASE_URL
pip install -r requirements.txt
uvicorn main:app --host 0.0.0.0 --port 5000 --reload
```

### Find the LAN IP (for firmware configuration, Mode 1)

```bash
ip route get 1 | awk '{print $7; exit}'
```

Set the result as `POST_URL` in `firmware/main/app_main.c`:
```
http://<LAN_IP>:5000/api/data
```

### Configuration

| Variable | Required | Example | Description |
|----------|----------|---------|-------------|
| `DATABASE_URL` | Yes | `postgresql://retro:retro@localhost:5432/readings` | PostgreSQL connection string. Copy `server/.env.example` to `server/.env` for local dev. |

### Database

Schema is created automatically at startup via `_init_db()`. No manual steps required.

---

## 2. Deployment modes

| Mode | Path | How to use |
|------|------|------------|
| **1 — Local** | Device → LAN → `localhost:5000` | `docker compose up` (or Option B). Firmware `POST_URL` = LAN IP. |
| **2 — Azure (dummy data)** | Seed script → Azure → Dashboard | Run `infra/azure-setup.sh` once, then CI/CD deploys on push to `main`. Use `server/seed.py` to generate test readings. |
| **3 — Azure (E2E)** | Device → Azure → Dashboard | Mode 2 deployed + firmware reflashed with HTTPS Azure URL. Cross-boundary atomic commit required — see `DEFERRED.md` §10. |

### Azure provisioning (run once)

```bash
chmod +x infra/azure-setup.sh
./infra/azure-setup.sh
```

The script creates: Azure Resource Group, PostgreSQL Flexible Server,
Container Registry, and Container Apps environment. It prints all values
needed for GitHub Secrets at the end.

### CI/CD (GitHub Actions)

`.github/workflows/deploy.yml` — triggers on push to `main` when files under
`server/` change. Builds the Docker image, pushes to ACR, deploys a new
Container App revision. Requires seven GitHub Secrets (printed by setup script).

### Dummy data (Mode 2 testing)

```bash
# Seed 60 readings to Azure
cd server
python seed.py --url https://<app-fqdn>/api/data

# Continuous streaming (simulates live device)
python seed.py --url https://<app-fqdn>/api/data --continuous --interval 30

# Target localhost
python seed.py --url http://localhost:5000/api/data --count 120
```

---

## 3. Server design

### Endpoints

| Method | Path | Description |
|--------|------|-------------|
| `POST` | `/api/data` | Ingest a batch of readings; persist to PostgreSQL; fan-out each reading to SSE subscribers |
| `GET` | `/api/data` | Query recent readings. Params: `device` (filter), `limit` (1–1000, default 100) |
| `GET` | `/api/stream` | SSE stream. Param: `device` (default `"retro-fit"`). One `asyncio.Queue` per connected client |
| `GET` | `/health` | Liveness check — returns `{"status": "ok"}` |
| `GET` | `/` | Serves the live visualiser UI (`server/static/index.html`) |

### Async model

FastAPI runs on an async event loop (uvicorn + asyncio). All endpoints are
`async def`. Database access uses `asyncpg` connection pool (min 2, max 10
connections). Pool is opened at startup and closed at shutdown via the FastAPI
`lifespan` context manager.

### SSE fan-out

`_sse_subscribers` maps `device → set[asyncio.Queue]`. When a POST lands:
1. Each inserted row is passed to `_publish(device, row)`
2. `_publish` calls `put_nowait` on each subscriber queue
3. If a queue is full (`maxsize=128`) the event is dropped — slow clients are
   skipped rather than allowed to block the POST handler

**Azure note:** Container App is configured with `min_replicas=1` to keep one
instance alive at all times. Scaling to zero would drop all open SSE connections.

### Pydantic models

| Model | Direction | Fields |
|-------|-----------|--------|
| `SingleReading` | Inbound (per reading) | `v: float\|null`, `t: int` |
| `BatchReading` | Inbound (full POST body) | `device: str`, `sensor: str`, `readings: list[SingleReading]` |
| `ReadingOut` | Outbound (GET + SSE) | `id`, `device`, `sensor_type`, `value`, `recorded_at` |

Wire format is defined in `API_CONTRACT.md` — do not change models without
following the Firmware ↔ Server notification rule.

### Timestamp reconstruction

The device sends `t` (ms since boot), not a wall-clock time. The server
reconstructs per-reading wall-clock times as:

```
recorded_at[i] = now - (max(t) - t[i]) ms
```

The most-recent reading in a batch gets `recorded_at = now`; earlier readings
are backdated by their relative offset.

---

## 4. Database design

### Engine

PostgreSQL 16 via `asyncpg`. Connection pool managed by FastAPI lifespan.
`DATABASE_URL` env var is required at startup.

### Schema

```sql
CREATE TABLE IF NOT EXISTS readings (
    id          BIGSERIAL    PRIMARY KEY,
    device      TEXT         NOT NULL,
    sensor_type TEXT         NOT NULL DEFAULT 'us',
    value       REAL,
    recorded_at TIMESTAMPTZ  NOT NULL
);

CREATE INDEX IF NOT EXISTS readings_device_recorded
ON readings (device, recorded_at DESC);
```

| Column | Type | Description |
|--------|------|-------------|
| `id` | BIGSERIAL PK | Auto-increment row ID |
| `device` | TEXT | Device identifier string (e.g. `"retro-fit"`) |
| `sensor_type` | TEXT | Short sensor tag from the `sensor` field in the batch POST (e.g. `"us"`) |
| `value` | REAL | Sensor reading; `NULL` on error/timeout |
| `recorded_at` | TIMESTAMPTZ | UTC timestamp reconstructed from device boot-relative offset |

### Migration strategy

`_init_db()` runs `CREATE TABLE IF NOT EXISTS` and `CREATE INDEX IF NOT EXISTS`
at every startup. It is idempotent and safe to run against an existing database.

Future schema changes (multi-tenant accounts/devices tables — see `DEFERRED.md`
§9) will be managed with Alembic once the schema stabilises (`DEFERRED.md` §8).

### Schema change log

| Date | Change | Author |
|------|--------|--------|
| 2026-04-20 | Initial schema: `id`, `device`, `distance_cm`, `recorded_at` (SQLite) | Server Agent |
| 2026-04-20 | `distance_cm` → `value`, add `sensor_type` (SQLite) | Server Agent |
| 2026-04-20 | Migrated to PostgreSQL (asyncpg); `recorded_at` TEXT → TIMESTAMPTZ; `id` INTEGER → BIGSERIAL; added `readings_device_recorded` index | Server Agent |

"""
HTTP logging service for ESP8266 retro-fit distance sensor device.

Endpoints:
  POST /api/data    - ingest a batch of sensor readings
  GET  /api/data    - retrieve recent readings
  GET  /api/stream  - SSE stream of new readings
  GET  /health      - liveness check
  GET  /            - live visualiser UI

Configuration (environment variables):
  DATABASE_URL      - PostgreSQL connection string (required)
                      e.g. postgresql://user:pass@host:5432/dbname
                      Local dev: copy server/.env.example to server/.env
"""

import asyncio
import json
import logging
import os
from contextlib import asynccontextmanager
from datetime import datetime, timedelta, timezone
from typing import Optional

import asyncpg
from dotenv import load_dotenv
from fastapi import FastAPI, Query
from fastapi.exceptions import RequestValidationError
from fastapi.responses import FileResponse, JSONResponse, StreamingResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, field_validator

load_dotenv()

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s  %(levelname)-8s  %(message)s",
    datefmt="%Y-%m-%dT%H:%M:%S",
)
logger = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Database
# ---------------------------------------------------------------------------

_pool: asyncpg.Pool | None = None


async def _init_db(pool: asyncpg.Pool) -> None:
    async with pool.acquire() as conn:
        await conn.execute(
            """
            CREATE TABLE IF NOT EXISTS readings (
                id          BIGSERIAL    PRIMARY KEY,
                device      TEXT         NOT NULL,
                sensor_type TEXT         NOT NULL DEFAULT 'us',
                value       REAL,
                recorded_at TIMESTAMPTZ  NOT NULL
            )
            """
        )
        await conn.execute(
            """
            CREATE INDEX IF NOT EXISTS readings_device_recorded
            ON readings (device, recorded_at DESC)
            """
        )
    logger.info("Database ready")


# ---------------------------------------------------------------------------
# SSE fan-out — one asyncio.Queue per connected client
# ---------------------------------------------------------------------------

_sse_subscribers: dict[str, set[asyncio.Queue]] = {}


def _publish(device: str, row: dict) -> None:
    queues = _sse_subscribers.get(device, set())
    payload = json.dumps(row)
    for q in list(queues):
        try:
            q.put_nowait(payload)
        except asyncio.QueueFull:
            pass  # slow client — drop rather than block


# ---------------------------------------------------------------------------
# App lifespan — pool open/close
# ---------------------------------------------------------------------------

@asynccontextmanager
async def lifespan(app: FastAPI):
    global _pool
    database_url = os.environ.get("DATABASE_URL")
    if not database_url:
        raise RuntimeError(
            "DATABASE_URL is not set. "
            "Copy server/.env.example to server/.env and fill in the value."
        )
    _pool = await asyncpg.create_pool(database_url, min_size=2, max_size=10)
    await _init_db(_pool)
    yield
    await _pool.close()


# ---------------------------------------------------------------------------
# Pydantic models
# ---------------------------------------------------------------------------

class SingleReading(BaseModel):
    v: Optional[float] = None   # sensor value; unit implied by batch-level sensor tag
    t: int                       # ms since device boot; used to reconstruct wall-clock time


class BatchReading(BaseModel):
    device: str
    sensor: str                  # short sensor-type tag, e.g. "us" for ultrasonic
    readings: list[SingleReading]

    @field_validator("device")
    @classmethod
    def device_not_empty(cls, v: str) -> str:
        if not v or not v.strip():
            raise ValueError("device must be a non-empty string")
        return v.strip()


class ReadingOut(BaseModel):
    id: int
    device: str
    sensor_type: str
    value: Optional[float]
    recorded_at: str


# ---------------------------------------------------------------------------
# App
# ---------------------------------------------------------------------------

app = FastAPI(title="ESP8266 Sensor Logger", version="1.0.0", lifespan=lifespan)

STATIC_DIR = ((__import__("pathlib").Path(__file__).parent) / "static")
if STATIC_DIR.is_dir():
    app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")


# ---------------------------------------------------------------------------
# POST /api/data
# ---------------------------------------------------------------------------

@app.post("/api/data", status_code=201)
async def ingest_reading(batch: BatchReading):
    if not batch.readings:
        return {"inserted": 0, "ids": []}

    now = datetime.now(timezone.utc)
    latest_ts = max(r.t for r in batch.readings)

    inserted = []
    async with _pool.acquire() as conn:
        async with conn.transaction():
            for r in batch.readings:
                delta_ms = latest_ts - r.t
                recorded_at = now - timedelta(milliseconds=delta_ms)
                row = await conn.fetchrow(
                    """
                    INSERT INTO readings (device, sensor_type, value, recorded_at)
                    VALUES ($1, $2, $3, $4)
                    RETURNING id
                    """,
                    batch.device, batch.sensor, r.v, recorded_at,
                )
                inserted.append({
                    "id": row["id"],
                    "device": batch.device,
                    "sensor_type": batch.sensor,
                    "value": r.v,
                    "recorded_at": recorded_at.isoformat(),
                })

    logger.info(
        "Batch of %d readings stored — device=%r  sensor=%r  span=%.1f s",
        len(inserted),
        batch.device,
        batch.sensor,
        (latest_ts - min(r.t for r in batch.readings)) / 1000.0,
    )

    for row in inserted:
        _publish(batch.device, row)

    return {"inserted": len(inserted), "ids": [row["id"] for row in inserted]}


# ---------------------------------------------------------------------------
# GET /api/data
# ---------------------------------------------------------------------------

@app.get("/api/data", response_model=list[ReadingOut])
async def get_readings(
    device: Optional[str] = Query(default=None, description="Filter by device name"),
    limit: int = Query(default=100, ge=1, le=1000, description="Max rows to return"),
):
    async with _pool.acquire() as conn:
        if device:
            rows = await conn.fetch(
                """
                SELECT id, device, sensor_type, value, recorded_at
                FROM readings
                WHERE device = $1
                ORDER BY recorded_at DESC
                LIMIT $2
                """,
                device, limit,
            )
        else:
            rows = await conn.fetch(
                """
                SELECT id, device, sensor_type, value, recorded_at
                FROM readings
                ORDER BY recorded_at DESC
                LIMIT $1
                """,
                limit,
            )

    return [
        {**dict(row), "recorded_at": row["recorded_at"].isoformat()}
        for row in rows
    ]


# ---------------------------------------------------------------------------
# GET /health
# ---------------------------------------------------------------------------

@app.get("/health")
async def health():
    return {"status": "ok"}


# ---------------------------------------------------------------------------
# GET /api/stream — Server-Sent Events
# ---------------------------------------------------------------------------

@app.get("/api/stream")
async def stream_readings(
    device: str = Query(default="retro-fit", description="Device name to subscribe to"),
):
    queue: asyncio.Queue = asyncio.Queue(maxsize=128)
    _sse_subscribers.setdefault(device, set()).add(queue)
    logger.info(
        "SSE client connected for device=%r  (total=%d)",
        device, len(_sse_subscribers[device]),
    )

    async def event_generator():
        try:
            while True:
                payload = await queue.get()
                yield f"data: {payload}\n\n"
        except asyncio.CancelledError:
            pass
        finally:
            _sse_subscribers[device].discard(queue)
            logger.info(
                "SSE client disconnected for device=%r  (remaining=%d)",
                device, len(_sse_subscribers.get(device, set())),
            )

    return StreamingResponse(
        event_generator(),
        media_type="text/event-stream",
        headers={
            "Cache-Control": "no-cache",
            "X-Accel-Buffering": "no",
        },
    )


# ---------------------------------------------------------------------------
# GET / — live visualiser
# ---------------------------------------------------------------------------

@app.get("/")
async def index():
    return FileResponse(STATIC_DIR / "index.html")


# ---------------------------------------------------------------------------
# Custom 422 handler
# ---------------------------------------------------------------------------

@app.exception_handler(RequestValidationError)
async def validation_exception_handler(request, exc: RequestValidationError):
    return JSONResponse(
        status_code=422,
        content={"detail": exc.errors()},
    )


# ---------------------------------------------------------------------------
# Entry point (for direct execution; normally use uvicorn CLI)
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    import uvicorn
    uvicorn.run("main:app", host="0.0.0.0", port=5000, reload=False)

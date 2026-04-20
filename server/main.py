"""
HTTP logging service for ESP8266 retro-fit distance sensor device.

Endpoints:
  POST /api/data    - ingest a sensor reading
  GET  /api/data    - retrieve recent readings
  GET  /api/stream  - SSE stream of new readings
  GET  /health      - liveness check
  GET  /            - live visualizer UI
"""

import asyncio
import json
import logging
import sqlite3
from contextlib import contextmanager
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Optional

from fastapi import FastAPI, Query
from fastapi.responses import JSONResponse, StreamingResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, field_validator

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

DB_PATH = Path(__file__).parent / "readings.db"


def _get_conn() -> sqlite3.Connection:
    conn = sqlite3.connect(DB_PATH, check_same_thread=False)
    conn.row_factory = sqlite3.Row
    return conn


@contextmanager
def get_db():
    conn = _get_conn()
    try:
        yield conn
        conn.commit()
    except Exception:
        conn.rollback()
        raise
    finally:
        conn.close()


def init_db() -> None:
    with get_db() as conn:
        conn.execute(
            """
            CREATE TABLE IF NOT EXISTS readings (
                id          INTEGER PRIMARY KEY AUTOINCREMENT,
                device      TEXT    NOT NULL,
                sensor_type TEXT    NOT NULL DEFAULT 'us',
                value       REAL,
                recorded_at TEXT    NOT NULL
            )
            """
        )
        # Migrate from the old schema (distance_cm column) to the new one.
        cols = {row[1] for row in conn.execute("PRAGMA table_info(readings)").fetchall()}
        if "distance_cm" in cols:
            logger.info("Migrating readings table to new schema")
            conn.executescript(
                """
                CREATE TABLE readings_new (
                    id          INTEGER PRIMARY KEY AUTOINCREMENT,
                    device      TEXT    NOT NULL,
                    sensor_type TEXT    NOT NULL DEFAULT 'us',
                    value       REAL,
                    recorded_at TEXT    NOT NULL
                );
                INSERT INTO readings_new (id, device, sensor_type, value, recorded_at)
                    SELECT id, device, 'us', distance_cm, recorded_at FROM readings;
                DROP TABLE readings;
                ALTER TABLE readings_new RENAME TO readings;
                """
            )
            logger.info("Migration complete")
    logger.info("Database ready at %s", DB_PATH)


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
# SSE fan-out — one asyncio.Queue per connected client
# ---------------------------------------------------------------------------

# Maps device name -> set of queues for connected SSE clients.
# Using a plain dict of sets; keys are created on first subscribe.
_sse_subscribers: dict[str, set[asyncio.Queue]] = {}


def _publish(device: str, row: dict) -> None:
    """Called from the synchronous POST handler to fan-out a new row."""
    queues = _sse_subscribers.get(device, set())
    payload = json.dumps(row)
    for q in list(queues):
        try:
            q.put_nowait(payload)
        except asyncio.QueueFull:
            pass  # slow client — skip rather than block


# ---------------------------------------------------------------------------
# App
# ---------------------------------------------------------------------------

app = FastAPI(title="ESP8266 Sensor Logger", version="1.0.0")

STATIC_DIR = Path(__file__).parent / "static"
if STATIC_DIR.is_dir():
    app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")


@app.on_event("startup")
def on_startup() -> None:
    init_db()


# ---------------------------------------------------------------------------
# POST /api/data
# ---------------------------------------------------------------------------


@app.post("/api/data", status_code=201)
def ingest_reading(batch: BatchReading):
    if not batch.readings:
        return {"inserted": 0, "ids": []}

    # The most-recent reading's t corresponds to "now" on the device.
    # Back-compute each reading's wall-clock time from the relative offsets so
    # that per-sample times are faithful even when readings arrive in a batch.
    now = datetime.now(timezone.utc)
    latest_ts = max(r.t for r in batch.readings)

    inserted = []
    with get_db() as conn:
        for r in batch.readings:
            delta_ms = latest_ts - r.t
            recorded_at = (now - timedelta(milliseconds=delta_ms)).isoformat()
            cursor = conn.execute(
                "INSERT INTO readings (device, sensor_type, value, recorded_at) VALUES (?, ?, ?, ?)",
                (batch.device, batch.sensor, r.v, recorded_at),
            )
            inserted.append({
                "id": cursor.lastrowid,
                "device": batch.device,
                "sensor_type": batch.sensor,
                "value": r.v,
                "recorded_at": recorded_at,
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
def get_readings(
    device: Optional[str] = Query(default=None, description="Filter by device name"),
    limit: int = Query(default=100, ge=1, le=1000, description="Max rows to return (1-1000)"),
):
    with get_db() as conn:
        if device:
            rows = conn.execute(
                "SELECT id, device, sensor_type, value, recorded_at "
                "FROM readings WHERE device = ? "
                "ORDER BY id DESC LIMIT ?",
                (device, limit),
            ).fetchall()
        else:
            rows = conn.execute(
                "SELECT id, device, sensor_type, value, recorded_at "
                "FROM readings ORDER BY id DESC LIMIT ?",
                (limit,),
            ).fetchall()

    return [dict(row) for row in rows]


# ---------------------------------------------------------------------------
# GET /health
# ---------------------------------------------------------------------------


@app.get("/health")
def health():
    return {"status": "ok"}


# ---------------------------------------------------------------------------
# GET /api/stream  — Server-Sent Events
# ---------------------------------------------------------------------------


@app.get("/api/stream")
async def stream_readings(
    device: str = Query(default="retro-fit", description="Device name to subscribe to"),
):
    queue: asyncio.Queue = asyncio.Queue(maxsize=128)

    # Register this client
    _sse_subscribers.setdefault(device, set()).add(queue)
    logger.info("SSE client connected for device=%r  (total=%d)", device, len(_sse_subscribers[device]))

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
                device,
                len(_sse_subscribers.get(device, set())),
            )

    return StreamingResponse(
        event_generator(),
        media_type="text/event-stream",
        headers={
            "Cache-Control": "no-cache",
            "X-Accel-Buffering": "no",  # disable nginx buffering if behind a proxy
        },
    )


# ---------------------------------------------------------------------------
# GET /  — live visualizer
# ---------------------------------------------------------------------------


@app.get("/")
async def index():
    from fastapi.responses import FileResponse
    return FileResponse(STATIC_DIR / "index.html")


# ---------------------------------------------------------------------------
# Custom 422 handler — surface Pydantic validation messages clearly
# ---------------------------------------------------------------------------


from fastapi.exceptions import RequestValidationError


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

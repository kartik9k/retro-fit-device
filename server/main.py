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
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

from fastapi import FastAPI, Query
from fastapi.responses import JSONResponse, StreamingResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, field_validator, model_validator

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
                distance_cm REAL,
                recorded_at TEXT    NOT NULL
            )
            """
        )
    logger.info("Database ready at %s", DB_PATH)


# ---------------------------------------------------------------------------
# Pydantic models
# ---------------------------------------------------------------------------


class SensorReading(BaseModel):
    device: str
    distance_cm: Optional[float] = None

    @model_validator(mode="before")
    @classmethod
    def check_distance_type(cls, values: dict) -> dict:
        """
        Reject payloads where distance_cm is present but is neither a
        number nor JSON null.  Pydantic would coerce strings like "abc"
        into a validation error, but we want a clear 422 with a message
        rather than a generic Pydantic error, so we intercept here.
        """
        if "distance_cm" in values:
            raw = values["distance_cm"]
            if raw is not None and not isinstance(raw, (int, float)):
                raise ValueError(
                    f"distance_cm must be a number or null, got {type(raw).__name__!r}"
                )
        return values

    @field_validator("device")
    @classmethod
    def device_not_empty(cls, v: str) -> str:
        if not v or not v.strip():
            raise ValueError("device must be a non-empty string")
        return v.strip()


class ReadingOut(BaseModel):
    id: int
    device: str
    distance_cm: Optional[float]
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
def ingest_reading(reading: SensorReading):
    recorded_at = datetime.now(timezone.utc).isoformat()

    with get_db() as conn:
        cursor = conn.execute(
            "INSERT INTO readings (device, distance_cm, recorded_at) VALUES (?, ?, ?)",
            (reading.device, reading.distance_cm, recorded_at),
        )
        row_id = cursor.lastrowid

    logger.info(
        "Reading #%d stored — device=%r  distance_cm=%s  recorded_at=%s",
        row_id,
        reading.device,
        reading.distance_cm if reading.distance_cm is not None else "null (out of range)",
        recorded_at,
    )

    # Fan-out to any connected SSE clients watching this device
    _publish(
        reading.device,
        {
            "id": row_id,
            "device": reading.device,
            "distance_cm": reading.distance_cm,
            "recorded_at": recorded_at,
        },
    )

    return {"id": row_id, "recorded_at": recorded_at}


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
                "SELECT id, device, distance_cm, recorded_at "
                "FROM readings WHERE device = ? "
                "ORDER BY id DESC LIMIT ?",
                (device, limit),
            ).fetchall()
        else:
            rows = conn.execute(
                "SELECT id, device, distance_cm, recorded_at "
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
    errors = exc.errors()
    # Look for a distance_cm type error and return the custom message
    for err in errors:
        loc = [str(l) for l in err.get("loc", [])]
        if "distance_cm" in loc:
            return JSONResponse(
                status_code=422,
                content={"detail": err["msg"]},
            )
    # Generic 400 for other payload problems (missing fields, wrong types, etc.)
    return JSONResponse(
        status_code=400,
        content={"detail": errors},
    )


# ---------------------------------------------------------------------------
# Entry point (for direct execution; normally use uvicorn CLI)
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    import uvicorn

    uvicorn.run("main:app", host="0.0.0.0", port=5000, reload=False)

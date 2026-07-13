# Deferred Work

Items here were explicitly scoped out during requirement triage. Each entry records
what was deferred, why, and what must be true before it can be picked up.

---

## 1. Real cellular AT-command driver (SIM7080G)

**Agreed module:** SIMCOM SIM7080G (NB-IoT + LTE-M)

**Why deferred:** The requirement asked only for a modular transport layer with
scaffolding. The stub in `main/cellular_transport.c` holds the correct interface
boundary; the real driver slots in there without touching any other file.

**What must be true before implementing:**
- Deep sleep architecture (item 2 below) must be designed first — the AT-command
  driver and the sleep/wake cycle are tightly coupled. Starting the driver without
  deep sleep produces a device that works but drains the battery in days, not months.
- Confirm SIM card / APN credentials provisioning story: captive-portal SoftAP
  works for Wi-Fi credentials but cellular APN config needs a separate solution
  (baked-in at build time, or a new provisioning flow).
- The UART0 pin-swap to GPIO13/GPIO15 must be called before cellular driver init;
  see HARDWARE.md § UART0 Pin Swap.
- Send `AT+IPR=115200` early in init to lock the baud rate in the module's NVM
  (prevents autobaud drift on noisy lines).
- PWRKEY timing: GPIO12 must be held low for ≥ 1 s to power on, ≥ 1.2 s to
  power off. The driver must implement this as a blocking GPIO pulse, not a
  fire-and-forget.

**Interface boundary:** `main/cellular_transport.c` — implement `.init`,
`.is_ready`, and `.post` there.

---

## 2. Deep sleep architecture

**Why deferred:** The current firmware runs two always-on FreeRTOS tasks
(`sensor_task` at priority 8, `post_task` at priority 5). This is correct and
efficient for Wi-Fi deployments where the device is mains-powered.

For cellular deployments targeting a 6-month battery life the device must enter
deep sleep between POST cycles. Deep sleep tears down all tasks and wakes via
the RTC timer — it is architecturally incompatible with the current task loop
without a redesign.

**Battery analysis from triage (reference):**

| POST interval | Avg current (ESP8266 + SIM7080G) | 6-month consumption |
|---|---|---|
| 30 s (current) | ~72 mA | 315 Ah — not feasible |
| 10 min | ~3.8 mA | 16.6 Ah — feasible |
| 15 min | ~2.5 mA | 11 Ah — comfortable |

A 2S 18650 Li-ion pack (~14 Ah usable) covers 6 months at a 15-minute interval.

**What must be true before implementing:**
- PM must confirm minimum acceptable POST interval for cellular deployments —
  this determines battery pack size and the sleep/wake cycle duration.
- The DYP-A22 driver uses a self-sustaining FreeRTOS timer pipeline that fires
  continuously at ~82 ms/cycle. Under deep sleep the pipeline must be replaced
  with a single-shot trigger-wait-read sequence per wake — the task architecture
  changes fundamentally.
- Hardware Agent must confirm that the SIM7080G PSM wake latency (time from
  deep sleep exit to first successful POST) fits within the desired window.

**Scope:** This is a significant firmware rework, not a small addition. Treat it
as a standalone requirement with its own triage cycle.

---

## 3. Per-transport POST interval configuration

**Why deferred:** `POST_PERIOD_US` is currently a single compile-time constant
(30 s). Wi-Fi deployments can keep 30 s; cellular deployments need 10–15 min
for battery viability.

This is blocked on deep sleep (item 2) — a configurable interval is meaningless
until the sleep/wake architecture supports it.

**Proposed mechanism (for when unblocked):** Two NVS keys (`post_period_wifi_s`,
`post_period_cellular_s`) read at boot, falling back to compile-time defaults.
The transport layer reports its type so `app_main` selects the right value.
No `API_CONTRACT.md` change required.

---

## 4. Runtime transport selection via NVS

**Why deferred:** `transport_get()` in `main/transport.c` currently always
returns `&wifi_transport`. The NVS-based runtime selection (key `transport/type`,
values `"wifi"` / `"cellular"`) is stubbed with a TODO comment there.

**Why not done now:** The cellular driver (item 1) is not implemented yet —
runtime selection of a non-functional driver adds no value and complicates
testing of the Wi-Fi path.

**What must be true before implementing:**
- Cellular driver (item 1) is implemented and tested.
- NVS initialisation in `app_main()` must happen before `transport_get()` is
  called. Currently NVS is initialised inside `wifi_manager_init()`. When runtime
  selection is added, move `nvs_flash_init()` to `app_main()` before the
  `transport_get()` call, and remove it from `wifi_manager_init()` (or make it
  idempotent).

---

## 5. Visualiser UX for sparse cellular update cadence

**Why deferred:** The live visualiser (`server/static/index.html`) renders a
continuous line chart. At a 15-minute POST interval the chart will show long
flat gaps between points, which looks broken to a user who does not know the
device is on cellular.

**Proposed UX (for when unblocked):** Add a "last updated X minutes ago"
indicator that refreshes on a 30-second client-side timer. No server changes
needed — the SSE stream already carries `recorded_at` timestamps.

**Blocked on:** Cellular driver being live in production so the actual UX
problem can be observed and validated before building the fix.

---

## 6. PostgreSQL migration (SQLite → cloud database)

**Status: IMPLEMENTED** — `server/main.py` now uses `asyncpg` against PostgreSQL.
`DATABASE_URL` env var required. See `SERVER.md` § Database design.

---

## 7. Cloud hosting — app server deployment

**Status: IMPLEMENTED** — Azure Container Apps (target: `retro-fit-dev` resource
group). Provisioning script at `infra/azure-setup.sh`. CI/CD via
`.github/workflows/deploy.yml`. Three deployment modes documented in `SERVER.md`.

**Firmware HTTPS (Mode 3) still pending** — see item 10 for the cross-boundary
atomic commit required when pointing devices at the Azure endpoint.

---

## 8. Schema versioning with Alembic

**Why deferred:** No migrations needed during development with SQLite — `init_db()`
recreates the table on schema changes.

**Required for production** so schema changes can be applied to a live database
without data loss.

**What changes:**
- `alembic init alembic` in `server/`.
- `env.py` wired to `DATABASE_URL`.
- Initial migration generated from the production schema (item 9).
- All future schema changes land as Alembic revision files, not ad-hoc ALTER
  TABLE statements.
- CI step to run `alembic upgrade head` on deploy.
- Update SERVER.md schema change log to reference Alembic revision IDs.

---

## 9. Multi-tenant database schema

**Why deferred:** Current schema has a single `readings` table keyed by a raw
`device` string. This is sufficient for development but cannot support multiple
customers with access-scoped data.

**Target schema (agreed during triage):**

```sql
CREATE TABLE accounts (
    id            SERIAL PRIMARY KEY,
    email         TEXT NOT NULL UNIQUE,
    password_hash TEXT NOT NULL,
    role          TEXT NOT NULL CHECK (role IN ('operator', 'customer')),
    created_at    TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE devices (
    id         SERIAL PRIMARY KEY,
    account_id INTEGER NOT NULL REFERENCES accounts(id),
    name       TEXT NOT NULL,
    device_key TEXT NOT NULL UNIQUE,  -- matches firmware "device" field
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE readings (
    id          BIGSERIAL PRIMARY KEY,
    device_id   INTEGER NOT NULL REFERENCES devices(id),
    sensor_type TEXT NOT NULL,
    value       REAL,
    recorded_at TIMESTAMPTZ NOT NULL
);

CREATE INDEX readings_device_recorded ON readings (device_id, recorded_at DESC);
```

**Auto-registration rule (PM decision):** On first POST from an unknown
`device_key`, the server creates a new `devices` row with `account_id = NULL`
(unowned). An operator can later claim it via the admin UI. This keeps firmware
zero-config while preserving the ability to scope data later.

**What changes in server:**
- `POST /api/data`: resolve `device` string → `device_id` via `devices` lookup;
  insert auto-registration row if not found.
- `GET /api/data`: join `readings → devices`; scope by `account_id` when the
  caller is authenticated as a customer.
- `GET /api/stream`: same scoping.
- Pydantic `ReadingOut` gains `device_name` field.

---

## 10. HTTPS in firmware

**Status: IMPLEMENTED** — ISRG Root X1 CA cert embedded as a string constant in
`firmware/main/wifi_transport.c`. `POST_URL` in `app_main.c` has both Mode 1
(LAN HTTP) and Mode 3 (Azure HTTPS) options as comments — uncomment the target
before building. `API_CONTRACT.md` updated with both base URLs.

---

## 11. Device API key authentication

**Why deferred:** No auth on `POST /api/data` during development. In production
an unauthenticated endpoint allows any device to inject readings.

**Design:**
- Each `devices` row gets a randomly generated `api_key` column (UUID or 32-byte
  hex, generated server-side at auto-registration or by operator).
- Firmware sends `Authorization: Bearer <api_key>` header on every POST.
- Server validates header; rejects with 401 if missing or invalid.
- `DEVICE_AUTH_REQUIRED` env flag (default `false`) — allows disabling during
  development without code changes.

**This is a cross-boundary change** — firmware must send the new header
simultaneously with the server enforcing it. Coordinate per Firmware ↔ Server
notification rule; land in one atomic commit covering `API_CONTRACT.md`, firmware
(`wifi_transport.c` and `cellular_transport.c`), and server.

**Firmware impact:** API key must be provisioned onto the device. Options:
1. Baked into firmware at build time (simplest; requires a per-device build).
2. Stored in NVS; written during captive-portal provisioning (adds a field to the
   credential form).
3. Stored in NVS; fetched from server after initial unauthenticated registration
   POST (bootstrap flow — avoids per-device builds).

Option 3 is recommended. Requires a new unauthenticated `POST /api/register`
endpoint that returns the `api_key`; subsequent POSTs use it.

---

## 12. JWT dashboard authentication

**Why deferred:** Dashboard is open during development (PM decision). Production
requires login for customer-facing views.

**Design:**
- `POST /auth/login` — accepts email/password, returns short-lived JWT access
  token in httpOnly cookie + long-lived refresh token in separate httpOnly cookie.
- `POST /auth/refresh` — rotates access token using refresh token.
- `POST /auth/logout` — clears both cookies.
- `role` in JWT claims: `operator` (sees all devices/accounts) or `customer`
  (sees only own devices).
- `GET /api/data` and `GET /api/stream`: unauthenticated during development
  (current behaviour); enforced scope when `DASHBOARD_AUTH_REQUIRED=true`.

**Dependencies:**
- Multi-tenant schema (item 9) must exist so `account_id` is available for
  scoping.
- bcrypt (or argon2) for password hashing.
- `python-jose` or `PyJWT` for token generation.

---

## 13. Role-based dashboard views

**Why deferred:** Current dashboard shows a single device stream. Production
needs operator and customer views.

**Operator view:**
- Device selector dropdown populated from `GET /api/devices` (all devices across
  all accounts).
- Ability to claim unowned auto-registered devices, assign to accounts.
- All readings visible regardless of account.

**Customer view:**
- Only devices belonging to the authenticated account appear.
- No cross-account data visible.

**Implementation note:** The SSE stream must carry `device_id` so the client can
filter without polling. Currently `device` (the raw string) is in the SSE
payload; update to `device_id` when the multi-tenant schema lands.

**Blocked on:** JWT auth (item 12) and multi-tenant schema (item 9).

---

## 14. 30-day rolling data retention cleanup

**Why deferred:** No data volume pressure during development.

**Design:**
- Background `asyncio` task started at server startup.
- Runs every 24 hours: `DELETE FROM readings WHERE recorded_at < now() - interval '30 days'`.
- Interval configurable via `DATA_RETENTION_DAYS` env var (default 30).
- Log a count of deleted rows at INFO level each run.
- No `API_CONTRACT.md` change required.

**What must be true before implementing:**
- PostgreSQL migration (item 6) done — the `interval` syntax is PostgreSQL-specific.
  The equivalent SQLite query uses `datetime('now', '-30 days')` but implementing
  this for SQLite is not worth the effort given the planned migration.
- Update SERVER.md in the same commit.

---

## 15. Protobuf wire format (replace JSON)

**Why deferred:** JSON is readable, already working, and costs nothing at the
current scale (Wi-Fi, one batch POST every 30 s, payload under 1 KB).

**Trigger condition:** Pick this up when **all three** of the following are true:
1. Cellular transport is live (DEFERRED §1) — per-byte billing makes payload
   size a measurable operating cost.
2. Post frequency has increased to the point where JSON field-name overhead is
   non-trivial relative to the payload body.
3. The device fleet has grown enough that a schema enforcement gap (malformed
   JSON reaching the server) has caused a real incident.

**What changes:**
- Define a `.proto` schema that replaces `API_CONTRACT.md` as the wire-format
  source of truth. `API_CONTRACT.md` becomes a human-readable summary that
  references the `.proto` file.
- Firmware uses `nanopb` (no dynamic allocation, fixed-size fields) to encode.
  `nanopb` is SDK/CPU-agnostic — it is the correct library choice here.
- Server uses `protobuf` Python package to decode. `Content-Type` changes to
  `application/x-protobuf`.
- This is a **breaking change** — firmware and server must cut over atomically.
  All devices in the field must be flashed before or simultaneously with the
  server deploy. Plan a maintenance window.

**What does NOT change:** the batched POST approach, the endpoint path, the
device identifier, or the event key registry — only the encoding of the body.

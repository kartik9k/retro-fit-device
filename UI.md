# UI Design Document

> **Owner: UI Developer Agent.**
> This document is the single source of truth for all dashboard design decisions,
> component architecture, and data-visualisation patterns.
> Update it in the same commit as any change that affects the content below.
>
> **Cross-boundary rule:** The UI consumes Server Agent's REST and SSE endpoints.
> Any endpoint additions or response-shape changes must be agreed with Server
> Agent first and land in one atomic commit per `CLAUDE.md` § UI ↔ Server.

---

## Project overview

Browser-based dashboards served as static files from `server/static/`. Currently
a single-page live visualiser (`index.html`). Future: role-differentiated views
for operators (all devices, fleet overview) and customers (own devices only).

---

## 1. Design principles

### Design thinking

The primary user need is **situational awareness** — an operator or customer
must be able to answer "is this tank low?" within 3 seconds of opening the
dashboard, without reading numbers.

1. **Progressive disclosure:** At-a-glance status first (level %, colour
   indicator), trend second (chart), raw numbers last (table/hover).
2. **Confidence signals:** Always show when the data was last updated. A stale
   reading is worse than no reading — make staleness visible.
3. **Fail visibly:** If the sensor value is `null` (timeout/error), show an
   explicit "sensor error" state — do not interpolate or silently drop the point.
4. **Mobile first:** Operators check tanks from a phone. Layouts must work at
   360 px viewport width before being extended for desktop.

### Data visualisation

| Chart | Use case | Notes |
|-------|----------|-------|
| Gauge / filled arc | Current level % — at-a-glance primary metric | Green > 50 %, amber 20–50 %, red < 20 % |
| Time-series line chart | Level trend over configurable window (1 h, 24 h, 7 d) | Gap in line = `null` reading; do not interpolate. Mark gaps explicitly |
| Sparkline | Compact per-device card in fleet overview | Last 2 hours; no axes |
| Status dot | Live connectivity indicator | Green = data received in last 2× `POST_PERIOD`; amber = stale; grey = never seen |

**Colour palette:** Accessible palette (WCAG AA contrast). Avoid red/green-only
encoding — pair colour with shape or text for colour-blind users.

---

## 2. Current implementation (`server/static/index.html`)

Single self-contained HTML file. Vanilla JS, no build step, no framework.
Chart rendered with [Chart.js](https://www.chartjs.org/) loaded from CDN.

### What it does now

- Connects to `GET /api/stream?device=retro-fit` (SSE).
- Appends each incoming reading to a Chart.js line chart (up to last 50 points).
- Updates a "current value" card with the latest reading.
- No device selector — hardcoded device name.
- No staleness indicator.
- No error/null handling.

### Known gaps (to address in upcoming iterations)

| Gap | Impact | Linked deferred item |
|-----|--------|----------------------|
| Hardcoded device name | Cannot switch between devices | DEFERRED.md §13 |
| No staleness indicator | Looks live even when device is offline | DEFERRED.md §5 |
| No null/error state rendering | Chart silently skips sensor errors | — |
| No mobile layout | Unusable on phones < 768 px | — |
| CDN dependency | Fails without internet; unsuitable for LAN-only deployments | — |

---

## 3. Component roadmap

### Phase 1 — Polish current single-device view (no auth, no multi-device)

Deliverable: a production-quality single-device dashboard that can be shipped
to an early customer.

| Component | Description |
|-----------|-------------|
| Level gauge | Filled arc showing % full; colour-coded thresholds |
| Trend chart | Multi-window line chart (1 h / 24 h / 7 d toggle); gaps shown explicitly; Chart.js or lightweight alternative |
| Status bar | Device name, last-updated relative time (refreshed every 30 s), connection state (SSE connected / reconnecting) |
| Null / error state | Distinct visual treatment for `null` readings — dashed line segment + "sensor error" annotation |
| Mobile layout | Single-column stacked layout; gauge prominent above the fold |
| Bundled chart lib | Vendor Chart.js into `server/static/vendor/` to remove CDN dependency |

**Server API needed (no new endpoints required):** Existing `GET /api/data`
(for historical load on window change) and `GET /api/stream` (for live updates).

### Phase 2 — Multi-device operator view (no auth)

Blocked on: multi-tenant schema (DEFERRED.md §9).

| Component | Description |
|-----------|-------------|
| Device selector | Dropdown / URL param `?device=<device_key>` — drives both chart and SSE subscription |
| Fleet overview grid | Card per device: sparkline + current level + status dot |
| Device API: `GET /api/devices` | New endpoint needed — raise with Server Agent before building |

### Phase 3 — Authenticated operator + customer views

Blocked on: JWT auth (DEFERRED.md §12) and role-based views (DEFERRED.md §13).

| Component | Description |
|-----------|-------------|
| Login page | Email + password form; submits to `POST /auth/login`; stores JWT in httpOnly cookie |
| Session guard | Redirects unauthenticated users to `/login`; refreshes token silently |
| Operator layout | Fleet grid (all accounts + devices), device claim UI for unowned auto-registered devices |
| Customer layout | Own devices only; same single-device view as Phase 1 |
| Role routing | Server returns `role` in JWT; client renders operator or customer layout accordingly |

---

## 4. API surface consumed by the UI

The UI Developer Agent must not build against undocumented fields. Raise any
new requirement with Server Agent before implementation.

| Endpoint | Used for | Shape reference |
|----------|----------|-----------------|
| `GET /api/stream?device=<key>` | Live SSE readings | `SERVER.md` § Endpoints; `API_CONTRACT.md` § SSE payload |
| `GET /api/data?device=<key>&limit=<n>` | Historical load on window toggle | `SERVER.md` § Endpoints |
| `GET /health` | Connection indicator polling fallback | `SERVER.md` § Endpoints |
| `POST /auth/login` *(Phase 3)* | Session creation | To be defined with Server Agent |
| `GET /api/devices` *(Phase 2)* | Fleet device list | To be defined with Server Agent |

---

## 5. File layout

```
server/static/
├── index.html          # Current single-device dashboard (Phase 1 target)
├── vendor/             # Bundled third-party libs (Chart.js etc.) — Phase 1
└── (future)
    ├── login.html      # Phase 3
    ├── operator.html   # Phase 3
    └── customer.html   # Phase 3 — or single app with role routing
```

---

## Change log

| Date | Change | Author |
|------|--------|--------|
| 2026-04-20 | Initial UI design document; component roadmap Phases 1–3 | UI Developer Agent |

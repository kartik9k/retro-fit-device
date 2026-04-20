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
- The sensor task busy-wait loop (`read_mm()` on HC-SR04) is incompatible with
  deep sleep wake cycles under ~100 ms. For infrequent reporting the sensor
  fires once per wake, not continuously — the task architecture changes
  fundamentally.
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

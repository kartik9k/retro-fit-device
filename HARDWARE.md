# Hardware Configuration

> **Owner: Hardware Agent.**
> This file is the single source of truth for all physical board configuration.
> No GPIO assignment, voltage level, or wiring detail in firmware may diverge from this document.
>
> **Change protocol:**
> - Hardware Agent: update this file first, then notify Firmware Agent of the delta before any firmware change is made.
> - Firmware Agent: if a firmware requirement needs a hardware tweak (different GPIO, level-shifter, timing constraint), raise it with Hardware Agent before touching firmware — do not assume wiring can change silently.

---

## Board

| Field | Value |
|-------|-------|
| MCU | ESP8266 (ESP-12E / NodeMCU-style) |
| SDK | ESP8266 RTOS SDK (bundled at `esp8266-rtos-sdk/`) |
| Flash | 4 MB |
| Operating voltage | 3.3 V logic, 5 V Vin rail available |

---

## Sensor — HC-SR04 Ultrasonic Distance

| Sensor pin | Board pin | Notes |
|------------|-----------|-------|
| VCC | Vin (5 V) | Sensor requires 5 V supply |
| GND | GND | |
| TRIG | GPIO5 | 3.3 V drive sufficient; direct connection |
| ECHO | GPIO4 | Sensor outputs 5 V — **must go through voltage divider** |

### ECHO voltage divider

```
ECHO (5 V)
    │
  [1 kΩ]
    │
    ├──── GPIO4  (3.3 V nominal at this node)
    │
  [2 kΩ]
    │
   GND
```

Divider ratio: 2/3 → 5 V × 0.667 = 3.33 V at GPIO4. Within ESP8266 absolute max of 3.6 V.

### Timing

| Parameter | Value | Source |
|-----------|-------|--------|
| Trigger pulse width | 10 µs | HC-SR04 datasheet |
| Max echo pulse (timeout) | 38 000 µs | Datasheet max; ~6.5 m range |
| Min cycle time | 60 ms | Datasheet; sensor needs settling time between measurements |

> **Firmware note:** `SENSOR_PERIOD_MS = 2000` in `app_main.c` far exceeds the 60 ms minimum — no issue. If the period is ever reduced below 60 ms, Hardware Agent must be consulted.

---

## GPIO Assignment Summary

| GPIO | Direction | Function | Notes |
|------|-----------|----------|-------|
| GPIO4 | Input | HC-SR04 ECHO | Via 1 kΩ / 2 kΩ divider |
| GPIO5 | Output | HC-SR04 TRIG | Direct, 3.3 V |

All other GPIO pins are currently unassigned.

---

## Change Log

| Date | Change | Raised by | Notified |
|------|--------|-----------|----------|
| 2026-04-20 | Initial hardware config documented | Hardware Agent | Firmware Agent |

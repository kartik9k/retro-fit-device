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

## Cellular Module — SIMCOM SIM7080G (NB-IoT / LTE-M)

> **Status: wiring and power design documented. Driver not yet implemented.**
> See `DEFERRED.md` §1 and §2 before starting firmware work.

| Field | Value |
|-------|-------|
| Module | SIMCOM SIM7080G |
| Interface | UART (AT commands), full-duplex |
| Supply voltage | 2.1 V – 3.6 V (3.3 V rail shared with ESP8266 — see Power Supply below) |
| Baud rate | 115200, locked via `AT+IPR=115200` during driver init |
| Peak current (TX burst) | ~500 mA |
| PSM sleep current | ~6 µA |

### PWRKEY timing

| Operation | GPIO12 state | Duration |
|-----------|-------------|----------|
| Power on  | LOW | ≥ 1 000 ms |
| Power off | LOW | ≥ 1 200 ms |

GPIO12 drives PWRKEY directly (SIM7080G internal pull-up ~10 kΩ to VBAT;
sink current ~0.38 mA — within ESP8266 GPIO sink limit of 12 mA).

---

## Power Supply

The ESP8266 onboard LDO (typically 300–500 mA) cannot supply the SIM7080G
during radio TX bursts. A dedicated 3.3 V regulator is required.

| Component | Spec |
|-----------|------|
| Regulator | 3.3 V LDO or buck, ≥ 600 mA continuous (e.g. AP2112K-3.3 or TLV75533) |
| Battery | 18650 Li-ion, single-cell 3.6 V nominal (or 2S pack with BMS for higher capacity) |
| ESP8266 + SIM7080G | Share the 3.3 V output rail |

---

## UART0 Pin Swap

The ESP8266 UART0 defaults to GPIO1 (TX) / GPIO3 (RX), which conflict with the
USB-serial adapter used for flashing and monitoring. For cellular, UART0 is
remapped in firmware after boot:

```
uart_set_pin(UART_NUM_0, 15 /*TX*/, 13 /*RX*/, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
```

This frees GPIO1/GPIO3 for the USB-serial adapter. The swap must be called
before any cellular AT commands are sent.

**Boot-mode constraint:** GPIO15 must be LOW at reset for the ESP8266 to boot
from flash. Fit a 10 kΩ pull-down resistor from GPIO15 to GND permanently on
the board. The UART TX swap is done in software after boot, so this is safe.

---

## GPIO Assignment Summary

| GPIO | Direction | Function | Notes |
|------|-----------|----------|-------|
| GPIO4 | Input | HC-SR04 ECHO | Via 1 kΩ / 2 kΩ voltage divider |
| GPIO5 | Output | HC-SR04 TRIG | Direct, 3.3 V |
| GPIO12 | Output | SIM7080G PWRKEY | Active-low pulse; direct drive |
| GPIO13 | Input | SIM7080G TX → ESP RX | UART0 RX after pin-swap |
| GPIO14 | Output | SIM7080G nRESET *(optional)* | Active-low |
| GPIO15 | Output | SIM7080G RX ← ESP TX | UART0 TX after pin-swap; **10 kΩ pull-down to GND required** |

**Unassigned:** GPIO0, GPIO1, GPIO2, GPIO3, GPIO16 (GPIO0/2 are boot-mode
sensitive; GPIO16 is deep-sleep wake — reserve for DEFERRED §2).

---

## Change Log

| Date | Change | Raised by | Notified |
|------|--------|-----------|----------|
| 2026-04-20 | Initial hardware config documented | Hardware Agent | Firmware Agent |
| 2026-04-20 | Added SIM7080G wiring, power supply, UART0 pin-swap, updated GPIO table | Hardware Agent | Firmware Agent |

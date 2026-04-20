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
| SDK | ESP8266 RTOS SDK (bundled at `firmware/esp8266-rtos-sdk/`) |
| Flash | 4 MB |
| Operating voltage | 3.3 V logic, 5 V Vin rail available |

---

## Hardware configurations

Two sensor configurations are maintained in parallel. The active configuration
is selected at build time by changing the sensor include and pointer in
`firmware/main/app_main.c` (see `FIRMWARE.md` § Switching sensor
configuration).

| | **v1 — HC-SR04** | **v2 — DYP-A22** |
|---|---|---|
| Status | **Active — in use** | Ordered — hardware not yet received |
| Sensor | HC-SR04 (non-waterproof) | DYP-A22 (IP67 waterproof) |
| Min range | 2 cm | 2 cm |
| Max range | 400 cm | 300 cm |
| Interface | TRIG / ECHO (pulse) | I2C |
| GPIO4 function | HC-SR04 ECHO (via voltage divider) | I2C SDA (direct, pull-up required) |
| GPIO5 function | HC-SR04 TRIG (direct) | I2C SCL (direct, pull-up required) |
| Voltage divider on GPIO4 | Required (1 kΩ / 2 kΩ) | **Must be removed** |
| I2C pull-ups | Not applicable | 4.7 kΩ × 2 to 3.3 V required |
| Supply | 5 V (Vin) | 3.3 V |
| Driver file | `firmware/main/hcsr04.c` | `firmware/main/dyp_a22.c` *(stub)* |

> **v2 note:** DYP-A22 driver is a stub returning `DISTANCE_SENSOR_ERR`.
> Full I2C implementation is pending hardware receipt and bench testing.
> See `firmware/main/dyp_a22.c` TODO block for the implementation checklist.

---

## v1 — HC-SR04 Ultrasonic Distance Sensor

### Wiring

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

> **Firmware note:** `SENSOR_PERIOD_MS = 2000` far exceeds the 60 ms minimum. If the period
> is ever reduced below 60 ms, Hardware Agent must be consulted.

---

## v2 — DYP-A22 Waterproof Ultrasonic Sensor (I2C mode)

> **Status: ordered, hardware not yet received. Board changes below apply
> when fitting the DYP-A22. Do not make these board changes until the unit
> has been verified on the bench.**

### Board changes required when migrating from v1

1. **Remove** the 1 kΩ / 2 kΩ voltage divider on GPIO4
2. **Fit** 4.7 kΩ pull-up resistor from GPIO4 (SDA) to 3.3 V
3. **Fit** 4.7 kΩ pull-up resistor from GPIO5 (SCL) to 3.3 V
4. **Change** sensor supply from 5 V (Vin) to 3.3 V rail
5. **Verify** DYP-A22 is configured for I2C output mode (hardware jumper or default — confirm on received unit)

### Wiring

| Sensor pin | Board pin | Notes |
|------------|-----------|-------|
| VCC | 3.3 V | Do **not** connect to 5 V Vin |
| GND | GND | |
| SDA | GPIO4 | 4.7 kΩ pull-up to 3.3 V on board |
| SCL | GPIO5 | 4.7 kΩ pull-up to 3.3 V on board |

### I2C parameters

| Parameter | Value | Notes |
|-----------|-------|-------|
| Default I2C address | TBC | Confirm from datasheet on received unit |
| Bus speed | 400 kHz | Standard fast-mode |
| Pull-up value | 4.7 kΩ | To 3.3 V; required — ESP8266 internal pull-ups insufficient for reliable I2C |

### Specifications

| Parameter | Value |
|-----------|-------|
| Min range (blind zone) | 2 cm |
| Max range | 300 cm |
| Active current | ~8–15 mA (confirm on received unit) |
| IP rating | IP67 |
| Supply voltage | 3.3 V |
| Response time | ~30 ms |

### I2C bus extensibility

GPIO4/GPIO5 form a shared I2C bus. Additional sensors (temperature, humidity,
barometric pressure, second ultrasonic) can be added to the same two wires.
Each device must have a unique I2C address — verify there are no address
conflicts before adding devices.

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

The active sensor configuration determines GPIO4 and GPIO5 function.

### v1 — HC-SR04 (active)

| GPIO | Direction | Function | Notes |
|------|-----------|----------|-------|
| GPIO4 | Input | HC-SR04 ECHO | Via 1 kΩ / 2 kΩ voltage divider |
| GPIO5 | Output | HC-SR04 TRIG | Direct, 3.3 V |
| GPIO12 | Output | SIM7080G PWRKEY | Active-low pulse; direct drive |
| GPIO13 | Input | SIM7080G TX → ESP RX | UART0 RX after pin-swap |
| GPIO14 | Output | SIM7080G nRESET *(optional)* | Active-low |
| GPIO15 | Output | SIM7080G RX ← ESP TX | UART0 TX after pin-swap; **10 kΩ pull-down to GND required** |

### v2 — DYP-A22 (pending)

| GPIO | Direction | Function | Notes |
|------|-----------|----------|-------|
| GPIO4 | Bidir | I2C SDA | 4.7 kΩ pull-up to 3.3 V; voltage divider **removed** |
| GPIO5 | Output | I2C SCL | 4.7 kΩ pull-up to 3.3 V |
| GPIO12 | Output | SIM7080G PWRKEY | Unchanged from v1 |
| GPIO13 | Input | SIM7080G TX → ESP RX | Unchanged from v1 |
| GPIO14 | Output | SIM7080G nRESET *(optional)* | Unchanged from v1 |
| GPIO15 | Output | SIM7080G RX ← ESP TX | Unchanged from v1 |

**Unassigned (both configs):** GPIO0, GPIO1, GPIO2, GPIO3, GPIO16
(GPIO0/2 are boot-mode sensitive; GPIO16 reserved for deep-sleep wake — see `DEFERRED.md` §2).

---

## Change Log

| Date | Change | Raised by | Notified |
|------|--------|-----------|----------|
| 2026-04-20 | Initial hardware config documented | Hardware Agent | Firmware Agent |
| 2026-04-20 | Added SIM7080G wiring, power supply, UART0 pin-swap, updated GPIO table | Hardware Agent | Firmware Agent |
| 2026-04-20 | Added v2 DYP-A22 configuration alongside v1 HC-SR04; dual GPIO tables | Hardware Agent | Firmware Agent |

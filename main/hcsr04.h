#pragma once

/*
 * HC-SR04 ultrasonic distance sensor driver.
 *
 * Wiring
 * ------
 *  VCC  → 5 V (Vin)
 *  GND  → GND
 *  TRIG → GPIO5  (direct, 3.3 V drive is sufficient)
 *  ECHO → GPIO4  via 1 kΩ / 2 kΩ voltage divider  (sensor outputs 5 V)
 *
 *              5 V
 *               │
 *          [1 kΩ]
 *               │
 *               ├──── GPIO4
 *               │
 *          [2 kΩ]
 *               │
 *              GND
 */

#include "distance_sensor.h"

extern const distance_sensor_t hcsr04_sensor;

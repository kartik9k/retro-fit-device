#pragma once

/*
 * DYP-A22 waterproof ultrasonic distance sensor driver — I2C mode.
 *
 * STATUS: stub only — hardware not yet received. init() returns
 * ESP_ERR_NOT_SUPPORTED and read_mm() returns DISTANCE_SENSOR_ERR until
 * the real driver is implemented on actual hardware.
 *
 * Wiring (Hardware v2 — see HARDWARE.md § Hardware configurations)
 * ----------------------------------------------------------------
 *  VCC  → 3.3 V
 *  GND  → GND
 *  SDA  → GPIO4  (4.7 kΩ pull-up to 3.3 V required on board)
 *  SCL  → GPIO5  (4.7 kΩ pull-up to 3.3 V required on board)
 *
 * Note: GPIO4/GPIO5 are the same pins used for HC-SR04 ECHO/TRIG.
 * The voltage divider on GPIO4 must be removed before fitting the DYP-A22.
 *
 * To activate this driver in app_main.c:
 *   Replace:  #include "hcsr04.h"
 *             static const distance_sensor_t *s_sensor = &hcsr04_sensor;
 *   With:     #include "dyp_a22.h"
 *             static const distance_sensor_t *s_sensor = &dyp_a22_sensor;
 */

#include "distance_sensor.h"

extern const distance_sensor_t dyp_a22_sensor;

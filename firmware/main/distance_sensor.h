#pragma once

#include <stdint.h>
#include "esp_err.h"

/*
 * Sensor-agnostic interface for distance measurement.
 *
 * To add a new sensor:
 *   1. Create <sensor_name>.h / <sensor_name>.c
 *   2. Implement init() and read_mm(), both with the signatures below
 *   3. Expose a const distance_sensor_t describing that driver
 *   4. In app_main.c, swap the included header and the sensor pointer
 */

typedef struct {
    /* One-time hardware initialisation. Returns ESP_OK on success. */
    esp_err_t (*init)(void);

    /*
     * Trigger a measurement and return the result in millimetres.
     * Returns DISTANCE_SENSOR_ERR on timeout or out-of-range.
     */
    int32_t (*read_mm)(void);
} distance_sensor_t;

#define DISTANCE_SENSOR_ERR  INT32_MIN

/*
 * Short sensor-type tag included in every batch POST under the "sensor" key.
 * Override this in the active sensor's header if needed; app_main.c reads it
 * via the #include of the active sensor header.
 */
#define SENSOR_TYPE_TAG  "us"   /* ultrasonic */

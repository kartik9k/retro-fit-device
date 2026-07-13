#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <limits.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Wire protocol version — bump minor on additive changes, major on breaking */
#define PROTO_VERSION        "1.0"

/* Reading sentinel — pass this mm value to represent a null/error reading */
#define TELEM_READING_ERR    INT32_MIN

/* Buffer capacities — change here to tune memory vs. retention trade-off */
#define READING_QUEUE_DEPTH  30
#define EVENT_QUEUE_DEPTH    16

/* Flush thresholds — post_task is notified immediately when either is reached */
#define READING_FLUSH_AT     25
#define EVENT_FLUSH_AT        8

/* JSON body buffer — sized for worst-case full buffers plus envelopes */
#define TELEM_BUF_SZ         2300

/* -------------------------------------------------------------------------- */
/* Event value type                                                            */
/* -------------------------------------------------------------------------- */

typedef enum { TELEM_BOOL, TELEM_INT, TELEM_STR } telem_type_t;

typedef struct {
    telem_type_t type;
    union {
        bool    b;
        int32_t i;
        struct {
            const char *ptr; /* must point to a string literal or static buffer */
            uint8_t     len; /* byte count, not including null terminator       */
        } s;
    };
} telem_val_t;

/* Constructors for string literals — sizeof(x)-1 resolves at compile time */
#define TELEM_BOOL(x)       ((telem_val_t){ .type = TELEM_BOOL, .b = (x) })
#define TELEM_INT(x)        ((telem_val_t){ .type = TELEM_INT,  .i = (x) })
#define TELEM_STR(x)        ((telem_val_t){ .type = TELEM_STR,  .s = { .ptr = (x), .len = sizeof(x) - 1 } })
#define TELEM_STR_L(p, l)   ((telem_val_t){ .type = TELEM_STR,  .s = { .ptr = (p), .len = (uint8_t)(l) } })

/* -------------------------------------------------------------------------- */
/* API                                                                         */
/* -------------------------------------------------------------------------- */

/*
 * Call once from app_main before any task uses push functions.
 *   post_task_handle — task to notify when a flush threshold is hit.
 *   device_id        — written into every POST body; must outlive the module.
 */
esp_err_t telemetry_init(TaskHandle_t post_task_handle, const char *device_id);

/*
 * Enqueue a sensor reading. key must be a string literal (e.g. SENSOR_TYPE_TAG).
 * Pass TELEM_READING_ERR for mm when the sensor returned an error.
 * If the buffer is full the oldest reading is silently evicted.
 * Thread-safe; call from task context only (not from an ISR).
 */
esp_err_t telemetry_push_reading(const char *key, int32_t mm, uint32_t timestamp_ms);

/*
 * Enqueue a device event. key and any string value pointer must remain valid
 * until the next telemetry_build_post call (string literals satisfy this).
 * If the buffer is full the oldest event is evicted with a warning log.
 * Thread-safe; call from task context only (not from an ISR).
 */
esp_err_t telemetry_push_event(const char *key, telem_val_t val, uint32_t timestamp_ms);

/*
 * Snapshot both buffers under mutex (clearing them), then build the full
 * JSON POST body into buf[len]. Returns total items consumed (readings +
 * events); 0 means nothing to send — caller should skip the POST.
 * Call exclusively from post_task.
 */
int telemetry_build_post(char *buf, size_t len);

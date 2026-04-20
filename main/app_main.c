#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "transport.h"

/* ---- active sensor: swap this include + pointer to change hardware ---- */
#include "hcsr04.h"
static const distance_sensor_t *s_sensor = &hcsr04_sensor;
/* ----------------------------------------------------------------------- */

/* ---------- user config ---------- */
#define POST_URL            "http://192.168.1.65:5000/api/data"
#define POST_PERIOD_US      (30ULL * 1000 * 1000)   /* 30 s */
#define SENSOR_PERIOD_MS    2000                     /* sample every 2 s */
#define READING_QUEUE_DEPTH 30                       /* ~60 s at 2 s/sample */
/* --------------------------------- */

static const char *TAG = "retro-fit";
static esp_timer_handle_t         s_post_timer;
static QueueHandle_t              s_reading_queue;
static TaskHandle_t               s_post_task_handle;
static const transport_driver_t  *s_transport;

/* Each queued reading carries a device-boot-relative timestamp so the server
 * can reconstruct wall-clock time for every sample in a batch. */
typedef struct {
    int32_t  distance_mm;
    uint32_t timestamp_ms;   /* ms since device boot; wraps ~49 days */
} reading_t;

/* ------------------------------------------------------------------ */
/*  Sensor task — runs continuously regardless of network state        */
/* ------------------------------------------------------------------ */

static void sensor_task(void *arg)
{
    for (;;) {
        reading_t r = {
            .distance_mm  = s_sensor->read_mm(),
            .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000),
        };
        if (r.distance_mm == DISTANCE_SENSOR_ERR) {
            ESP_LOGW(TAG, "sensor: no reading / out of range");
        } else {
            ESP_LOGI(TAG, "Distance: %"PRId32".%"PRId32" cm",
                     r.distance_mm / 10, r.distance_mm % 10);
        }
        /* When the queue is full, evict the oldest reading so the newest
         * data is always preserved. */
        if (xQueueSend(s_reading_queue, &r, 0) != pdTRUE) {
            reading_t discard;
            xQueueReceive(s_reading_queue, &discard, 0);
            xQueueSend(s_reading_queue, &r, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(SENSOR_PERIOD_MS));
    }
}

/* ------------------------------------------------------------------ */
/*  HTTP batch POST                                                     */
/* ------------------------------------------------------------------ */

/*
 * Per-reading budget: {"v":650.0,"t":4294967295}, = 27 chars worst-case.
 * Envelope: {"device":"retro-fit","sensor":"us","readings":[]} = 50 chars.
 * 30 × 27 + 50 = 860 → 900 gives comfortable headroom.
 */
#define BATCH_BUF_SZ  900

static void http_post_batch(const reading_t *batch, int count)
{
    static char body[BATCH_BUF_SZ];

    int pos = snprintf(body, sizeof(body),
                       "{\"device\":\"retro-fit\",\"sensor\":\"%s\",\"readings\":[",
                       SENSOR_TYPE_TAG);
    for (int i = 0; i < count; i++) {
        const reading_t *r = &batch[i];
        /* Reserve 4 bytes for closing "]}" and null terminator */
        int room = (int)sizeof(body) - pos - 4;
        if (room <= 0) {
            ESP_LOGW(TAG, "batch truncated at %d/%d readings", i, count);
            break;
        }
        if (r->distance_mm == DISTANCE_SENSOR_ERR) {
            pos += snprintf(body + pos, room,
                            "{\"v\":null,\"t\":%"PRIu32"}",
                            r->timestamp_ms);
        } else {
            pos += snprintf(body + pos, room,
                            "{\"v\":%"PRId32".%"PRId32",\"t\":%"PRIu32"}",
                            r->distance_mm / 10, r->distance_mm % 10,
                            r->timestamp_ms);
        }
        if (i < count - 1) body[pos++] = ',';
    }
    snprintf(body + pos, sizeof(body) - pos, "]}");

    esp_err_t err = s_transport->post(POST_URL, body, strlen(body));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "batch of %d readings flushed", count);
    }
}

/* ------------------------------------------------------------------ */
/*  POST timer callback — signals post_task; never blocks              */
/* ------------------------------------------------------------------ */

static void post_timer_cb(void *arg)
{
    if (!s_transport->is_ready()) {
        ESP_LOGW(TAG, "transport not ready, skipping POST");
        return;
    }
    xTaskNotifyGive(s_post_task_handle);
}

/* ------------------------------------------------------------------ */
/*  POST task — drains the reading queue and fires one batch POST      */
/* ------------------------------------------------------------------ */

static void post_task(void *arg)
{
    static reading_t batch[READING_QUEUE_DEPTH];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        int count = 0;
        while (count < READING_QUEUE_DEPTH &&
               xQueueReceive(s_reading_queue, &batch[count], 0) == pdTRUE) {
            count++;
        }
        if (count > 0) {
            http_post_batch(batch, count);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Entry point                                                         */
/* ------------------------------------------------------------------ */

void app_main(void)
{
    ESP_LOGI(TAG, "retro-fit-device starting");

    s_reading_queue = xQueueCreate(READING_QUEUE_DEPTH, sizeof(reading_t));
    configASSERT(s_reading_queue);

    s_transport = transport_get();

    ESP_ERROR_CHECK(s_sensor->init());
    /* Priority 8: above httpd/dns (5) so the echo pulse busy-wait is not
     * preempted mid-measurement; well below WiFi stack tasks (~23). */
    xTaskCreate(sensor_task, "sensor", 2048, NULL, 8, NULL);
    /* Priority 5: HTTP can block for seconds without affecting the sensor. */
    xTaskCreate(post_task, "post", 4096, NULL, 5, &s_post_task_handle);

    ESP_ERROR_CHECK(s_transport->init());

    const esp_timer_create_args_t timer_args = {
        .callback = post_timer_cb,
        .name     = "post_timer",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_post_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_post_timer, POST_PERIOD_US));
}

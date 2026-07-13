#include <stdint.h>
#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "transport.h"
#include "telemetry.h"

/* ---- active sensor: swap this include + pointer to change hardware ---- */
// v1 — HC-SR04 (deactivated — hardware removed):
// #include "hcsr04.h"
// static const distance_sensor_t *s_sensor = &hcsr04_sensor;

// v2 — DYP-A22 (active):
#include "dyp_a22.h"
static const distance_sensor_t *s_sensor = &dyp_a22_sensor;
/* ----------------------------------------------------------------------- */

/* ---------- user config ----------
 * POST_URL: uncomment the line that matches your deployment mode.
 *   Mode 1 — LAN  : device and server on the same Wi-Fi network.
 *   Mode 3 — Azure: device posts over the internet to the cloud server.
 */
// #define POST_URL  "http://192.168.1.65:5000/api/data"           /* Mode 1 — LAN  */
#define POST_URL     "https://retro-fit-server.nicepebble-7757b674.uksouth.azurecontainerapps.io/api/data"  /* Mode 3 — Azure */

#define POST_PERIOD_US   (30ULL * 1000 * 1000)  /* 30 s — periodic flush regardless of threshold */
#define SENSOR_PERIOD_MS 2000                    /* application sample interval                   */
/* --------------------------------- */

static const char *TAG = "retro-fit";
static esp_timer_handle_t        s_post_timer;
static TaskHandle_t              s_post_task_handle;
static const transport_driver_t *s_transport;

/* ------------------------------------------------------------------ */
/*  Sensor task — runs continuously regardless of network state        */
/* ------------------------------------------------------------------ */

static void sensor_task(void *arg)
{
    for (;;) {
        int32_t  mm = s_sensor->read_mm();
        uint32_t ts = (uint32_t)(esp_timer_get_time() / 1000);

        if (mm == DISTANCE_SENSOR_ERR) {
            ESP_LOGW(TAG, "sensor: no reading / out of range");
        } else {
            ESP_LOGI(TAG, "Distance: %"PRId32".%"PRId32" cm", mm / 10, mm % 10);
        }

        telemetry_push_reading(SENSOR_TYPE_TAG,
                               mm == DISTANCE_SENSOR_ERR ? TELEM_READING_ERR : mm,
                               ts);

        vTaskDelay(pdMS_TO_TICKS(SENSOR_PERIOD_MS));
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
/*  POST task — builds and sends one batch on each notification        */
/* ------------------------------------------------------------------ */

static void post_task(void *arg)
{
    static char body[TELEM_BUF_SZ];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        int items = telemetry_build_post(body, sizeof(body));
        if (items == 0) continue;  /* threshold fired but nothing accumulated */

        esp_err_t err = s_transport->post(POST_URL, body, strlen(body));
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "flushed %d items", items);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Entry point                                                         */
/* ------------------------------------------------------------------ */

void app_main(void)
{
    ESP_LOGI(TAG, "retro-fit-device starting");

    s_transport = transport_get();

    /* post_task must exist before telemetry_init so the handle is valid */
    xTaskCreate(post_task, "post", 4096, NULL, 5, &s_post_task_handle);
    ESP_ERROR_CHECK(telemetry_init(s_post_task_handle, "retro-fit"));

    ESP_ERROR_CHECK(s_sensor->init());
    /* Priority 8: retained from HC-SR04; safe to lower once DYP-A22 driver
     * is validated (I2C has no busy-wait — see FIRMWARE.md). */
    xTaskCreate(sensor_task, "sensor", 2048, NULL, 8, NULL);

    ESP_ERROR_CHECK(s_transport->init());

    const esp_timer_create_args_t timer_args = {
        .callback = post_timer_cb,
        .name     = "post_timer",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_post_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_post_timer, POST_PERIOD_US));
}

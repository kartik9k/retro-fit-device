#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"

#include "lwip/err.h"
#include "lwip/sys.h"

/* ---- active sensor: swap this include + pointer to change hardware ---- */
#include "hcsr04.h"
static const distance_sensor_t *s_sensor = &hcsr04_sensor;
/* ----------------------------------------------------------------------- */

/* ---------- user config ---------- */
#define WIFI_SSID           "YOUR_WIFI_SSID"
#define WIFI_PASS           "YOUR_WIFI_PASS"
#define POST_URL            "http://192.168.1.100:5000/api/data"  /* TODO: set to server LAN IP */
#define POST_PERIOD_US      (30ULL * 1000 * 1000)   /* 30 s */
#define SENSOR_PERIOD_MS    2000                     /* sample every 2 s */
/* --------------------------------- */

static const char *TAG = "retro-fit";
static esp_timer_handle_t s_post_timer;

/* Latest reading in mm, INT32_MIN when invalid. Written by sensor_task,
 * read by post_timer_cb. Atomic on single-core ESP8266 for 32-bit values. */
static volatile int32_t s_distance_mm = DISTANCE_SENSOR_ERR;

/* ------------------------------------------------------------------ */
/*  Sensor task — runs continuously regardless of network state        */
/* ------------------------------------------------------------------ */

static void sensor_task(void *arg)
{
    for (;;) {
        int32_t mm = s_sensor->read_mm();
        s_distance_mm = mm;
        if (mm == DISTANCE_SENSOR_ERR) {
            ESP_LOGW(TAG, "sensor: no reading / out of range");
        } else {
            ESP_LOGI(TAG, "Distance: %"PRId32".%"PRId32" cm",
                     mm / 10, mm % 10);
        }
        vTaskDelay(pdMS_TO_TICKS(SENSOR_PERIOD_MS));
    }
}

/* ------------------------------------------------------------------ */
/*  Wi-Fi                                                               */
/* ------------------------------------------------------------------ */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&evt->ip_info.ip));
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Disconnected, reconnecting...");
        esp_wifi_connect();
    }
}

static void wifi_init(void)
{
    tcpip_adapter_init();
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               wifi_event_handler, NULL));

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid     = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
}

/* ------------------------------------------------------------------ */
/*  HTTP POST                                                           */
/* ------------------------------------------------------------------ */

static void http_post(int32_t distance_mm)
{
    char body[64];
    if (distance_mm == DISTANCE_SENSOR_ERR) {
        snprintf(body, sizeof(body),
                 "{\"device\":\"retro-fit\",\"distance_cm\":null}");
    } else {
        snprintf(body, sizeof(body),
                 "{\"device\":\"retro-fit\",\"distance_cm\":%"PRId32".%"PRId32"}",
                 distance_mm / 10, distance_mm % 10);
    }

    esp_http_client_config_t cfg = {
        .url    = POST_URL,
        .method = HTTP_METHOD_POST,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "POST %d — %s",
                 esp_http_client_get_status_code(client), body);
    } else {
        ESP_LOGE(TAG, "POST failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
}

/* ------------------------------------------------------------------ */
/*  POST timer callback — network only, reads latest stored reading    */
/* ------------------------------------------------------------------ */

static void post_timer_cb(void *arg)
{
    tcpip_adapter_ip_info_t ip;
    if (tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip) != ESP_OK
            || ip.ip.addr == 0) {
        ESP_LOGW(TAG, "No IP yet, skipping POST");
        return;
    }
    http_post(s_distance_mm);
}

/* ------------------------------------------------------------------ */
/*  Entry point                                                         */
/* ------------------------------------------------------------------ */

void app_main(void)
{
    ESP_LOGI(TAG, "retro-fit-device starting");

    ESP_ERROR_CHECK(s_sensor->init());
    xTaskCreate(sensor_task, "sensor", 2048, NULL, 5, NULL);

    wifi_init();

    const esp_timer_create_args_t timer_args = {
        .callback = post_timer_cb,
        .name     = "post_timer",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_post_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_post_timer, POST_PERIOD_US));
}

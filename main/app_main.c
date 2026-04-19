#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "rom/ets_sys.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"

#include "lwip/err.h"
#include "lwip/sys.h"

/* ---------- user config ---------- */
#define WIFI_SSID           "YOUR_WIFI_SSID"
#define WIFI_PASS           "YOUR_WIFI_PASS"
#define POST_URL            "http://yourserver.com/api/data"
#define POST_PERIOD_US      (30ULL * 1000 * 1000)   /* 30 s */
#define SENSOR_PERIOD_MS    2000                     /* sample every 2 s */
/* --------------------------------- */

/* ---------- HC-SR04 config -------
 * TRIG: GPIO5  → direct connection  (3.3 V output)
 * ECHO: GPIO4  → via 1 kΩ/2 kΩ voltage divider from 5 V echo line
 * --------------------------------- */
#define HCSR04_TRIG_GPIO    GPIO_NUM_5
#define HCSR04_ECHO_GPIO    GPIO_NUM_4
#define HCSR04_TIMEOUT_US   38000           /* ~6.5 m max; datasheet max pulse */
#define SOUND_SPEED_CM_US   0.034320f       /* cm per µs at 20 °C */

static const char *TAG = "retro-fit";
static esp_timer_handle_t s_post_timer;

/* Latest distance reading, updated by sensor_task independent of Wi-Fi.
 * -1.0 means no echo / out of range. Single-core ESP8266: 32-bit aligned
 * float write is atomic, so no mutex needed for this scalar. */
static volatile float s_distance_cm = -1.0f;

/* ------------------------------------------------------------------ */
/*  HC-SR04 driver                                                      */
/* ------------------------------------------------------------------ */

static void hcsr04_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << HCSR04_TRIG_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(HCSR04_TRIG_GPIO, 0);

    io.pin_bit_mask = (1ULL << HCSR04_ECHO_GPIO);
    io.mode         = GPIO_MODE_INPUT;
    gpio_config(&io);
}

/* Returns distance in cm, or -1.0 on timeout / out-of-range. */
static float hcsr04_read_cm(void)
{
    /* 10 µs trigger pulse */
    gpio_set_level(HCSR04_TRIG_GPIO, 1);
    ets_delay_us(10);
    gpio_set_level(HCSR04_TRIG_GPIO, 0);

    /* Wait for echo to go HIGH */
    int64_t t0 = esp_timer_get_time();
    while (gpio_get_level(HCSR04_ECHO_GPIO) == 0) {
        if ((esp_timer_get_time() - t0) > HCSR04_TIMEOUT_US)
            return -1.0f;
    }
    int64_t echo_start = esp_timer_get_time();

    /* Wait for echo to go LOW */
    while (gpio_get_level(HCSR04_ECHO_GPIO) == 1) {
        if ((esp_timer_get_time() - echo_start) > HCSR04_TIMEOUT_US)
            return -1.0f;
    }
    int64_t echo_end = esp_timer_get_time();

    float duration_us = (float)(echo_end - echo_start);
    return duration_us * SOUND_SPEED_CM_US / 2.0f;
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

static void http_post(float distance_cm)
{
    char body[64];
    if (distance_cm < 0.0f) {
        snprintf(body, sizeof(body),
                 "{\"device\":\"retro-fit\",\"distance_cm\":null}");
    } else {
        snprintf(body, sizeof(body),
                 "{\"device\":\"retro-fit\",\"distance_cm\":%.1f}", distance_cm);
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
/*  Sensor task — runs continuously regardless of network state        */
/* ------------------------------------------------------------------ */

static void sensor_task(void *arg)
{
    for (;;) {
        float dist = hcsr04_read_cm();
        s_distance_cm = dist;
        if (dist < 0.0f) {
            ESP_LOGW(TAG, "HC-SR04: no echo / out of range");
        } else {
            ESP_LOGI(TAG, "Distance: %.1f cm", dist);
        }
        vTaskDelay(pdMS_TO_TICKS(SENSOR_PERIOD_MS));
    }
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
    http_post(s_distance_cm);
}

/* ------------------------------------------------------------------ */
/*  Entry point                                                         */
/* ------------------------------------------------------------------ */

void app_main(void)
{
    ESP_LOGI(TAG, "retro-fit-device starting");

    hcsr04_init();
    xTaskCreate(sensor_task, "sensor", 2048, NULL, 5, NULL);

    wifi_init();

    const esp_timer_create_args_t timer_args = {
        .callback = post_timer_cb,
        .name     = "post_timer",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_post_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_post_timer, POST_PERIOD_US));
}

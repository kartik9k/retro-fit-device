#include "wifi_manager.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "tcpip_adapter.h"

#define TAG             "wifi_mgr"
#define NVS_NAMESPACE   "wifi_mgr"
#define NVS_KEY_SSID    "ssid"
#define NVS_KEY_PASS    "pass"

#define WM_SSID_LEN     33      /* 32 chars + null */
#define WM_PASS_LEN     65      /* 64 chars + null */
#define POST_BODY_MAX   192     /* enough for url-encoded ssid + pass */

#define CONNECTED_BIT   BIT0

static EventGroupHandle_t s_event_group;

/* ------------------------------------------------------------------ */
/*  NVS helpers                                                         */
/* ------------------------------------------------------------------ */

static esp_err_t nvs_load(char *ssid, char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    size_t ssid_len = WM_SSID_LEN;
    size_t pass_len = WM_PASS_LEN;
    err = nvs_get_str(h, NVS_KEY_SSID, ssid, &ssid_len);
    if (err == ESP_OK)
        err = nvs_get_str(h, NVS_KEY_PASS, pass, &pass_len);

    nvs_close(h);
    return err;
}

static esp_err_t nvs_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    if ((err = nvs_set_str(h, NVS_KEY_SSID, ssid)) == ESP_OK)
        err = nvs_set_str(h, NVS_KEY_PASS, pass);
    if (err == ESP_OK)
        err = nvs_commit(h);

    nvs_close(h);
    return err;
}

/* ------------------------------------------------------------------ */
/*  URL decode (for application/x-www-form-urlencoded POST body)       */
/* ------------------------------------------------------------------ */

static void url_decode(const char *src, char *dst, size_t dst_max)
{
    size_t i = 0;
    while (*src && i < dst_max - 1) {
        if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], '\0' };
            dst[i++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

/* Extract the value of `key` from a url-encoded string like "a=1&b=2". */
static bool form_get(const char *body, const char *key,
                     char *out, size_t out_max)
{
    size_t klen = strlen(key);
    const char *p = body;
    while (*p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            const char *end = strchr(p, '&');
            size_t vlen = end ? (size_t)(end - p) : strlen(p);
            char raw[POST_BODY_MAX];
            vlen = vlen < sizeof(raw) - 1 ? vlen : sizeof(raw) - 1;
            memcpy(raw, p, vlen);
            raw[vlen] = '\0';
            url_decode(raw, out, out_max);
            return true;
        }
        p = strchr(p, '&');
        if (!p) break;
        p++;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/*  Wi-Fi event handler                                                 */
/* ------------------------------------------------------------------ */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Disconnected — reconnecting");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_event_group, CONNECTED_BIT);
    }
}

/* ------------------------------------------------------------------ */
/*  STA connect (normal boot)                                           */
/* ------------------------------------------------------------------ */

static esp_err_t sta_connect(const char *ssid, const char *pass)
{
    ESP_LOGI(TAG, "Connecting to \"%s\"", ssid);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               wifi_event_handler, NULL));

    wifi_config_t wifi_cfg = { 0 };
    strlcpy((char *)wifi_cfg.sta.ssid,     ssid, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, pass, sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    xEventGroupWaitBits(s_event_group, CONNECTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Provisioning — HTTP handlers                                        */
/* ------------------------------------------------------------------ */

static const char PROV_HTML[] =
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>retro-fit setup</title>"
    "<style>body{font-family:sans-serif;max-width:360px;margin:40px auto;padding:0 16px}"
    "input{width:100%;box-sizing:border-box;padding:8px;margin:6px 0 16px}"
    "button{width:100%;padding:10px;background:#0070f3;color:#fff;border:none;border-radius:4px;font-size:1em}"
    "</style></head><body>"
    "<h2>Wi&#8209;Fi Setup</h2>"
    "<form method='POST' action='/connect'>"
    "<label>Network SSID<br><input name='ssid' type='text' maxlength='32' required></label>"
    "<label>Password<br><input name='password' type='password' maxlength='64'></label>"
    "<button type='submit'>Save &amp; Connect</button>"
    "</form></body></html>";

static const char PROV_OK_HTML[] =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<title>Saved</title></head><body>"
    "<h2>Credentials saved</h2>"
    "<p>The device is restarting and will connect to your network.</p>"
    "</body></html>";

static esp_err_t handle_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PROV_HTML, strlen(PROV_HTML));
    return ESP_OK;
}

static esp_err_t handle_post(httpd_req_t *req)
{
    char body[POST_BODY_MAX] = { 0 };
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    body[received] = '\0';

    char ssid[WM_SSID_LEN] = { 0 };
    char pass[WM_PASS_LEN] = { 0 };

    if (!form_get(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
        httpd_resp_set_status(req, HTTPD_400);
        httpd_resp_send(req, "SSID is required", strlen("SSID is required"));
        return ESP_FAIL;
    }
    form_get(body, "password", pass, sizeof(pass));   /* password may be empty */

    esp_err_t err = nvs_save(ssid, pass);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS save failed: %s", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Credentials saved for \"%s\" — restarting", ssid);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PROV_OK_HTML, strlen(PROV_OK_HTML));

    vTaskDelay(pdMS_TO_TICKS(1000));    /* let the response flush */
    esp_restart();
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Provisioning — start SoftAP + HTTP server, then block forever      */
/* ------------------------------------------------------------------ */

static void start_provisioning(void)
{
    /* Build SSID from last 3 bytes of AP MAC  →  "retro-fit-AABBCC" */
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    char ap_ssid[24];
    snprintf(ap_ssid, sizeof(ap_ssid), "retro-fit-%02X%02X%02X",
             mac[3], mac[4], mac[5]);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap_cfg = {
        .ap = {
            .max_connection = 4,
            .authmode       = WIFI_AUTH_OPEN,
        },
    };
    strlcpy((char *)ap_cfg.ap.ssid, ap_ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen(ap_ssid);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Provisioning AP: \"%s\"  →  open http://192.168.4.1",
             ap_ssid);

    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server;
    ESP_ERROR_CHECK(httpd_start(&server, &http_cfg));

    httpd_uri_t get_uri  = { .uri = "/",        .method = HTTP_GET,  .handler = handle_get  };
    httpd_uri_t post_uri = { .uri = "/connect", .method = HTTP_POST, .handler = handle_post };
    httpd_register_uri_handler(server, &get_uri);
    httpd_register_uri_handler(server, &post_uri);

    /* Block here — handle_post calls esp_restart() when done */
    vTaskDelay(portMAX_DELAY);
}

/* ------------------------------------------------------------------ */
/*  Public entry point                                                  */
/* ------------------------------------------------------------------ */

esp_err_t wifi_manager_init(void)
{
    s_event_group = xEventGroupCreate();

    /* NVS init — erase if partition has changed layout */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition changed, erasing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    tcpip_adapter_init();
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    char ssid[WM_SSID_LEN] = { 0 };
    char pass[WM_PASS_LEN] = { 0 };

    if (nvs_load(ssid, pass) == ESP_OK && ssid[0] != '\0') {
        return sta_connect(ssid, pass);
    }

    ESP_LOGI(TAG, "No credentials in NVS — entering provisioning mode");
    start_provisioning();   /* never returns */
    return ESP_FAIL;
}

#include <stdbool.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "tcpip_adapter.h"

#include "wifi_manager.h"
#include "transport.h"

static const char *TAG = "wifi_transport";

static esp_err_t wifi_init(void)
{
    return wifi_manager_init();
}

static bool wifi_is_ready(void)
{
    tcpip_adapter_ip_info_t ip;
    return tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip) == ESP_OK
           && ip.ip.addr != 0;
}

static esp_err_t wifi_post(const char *url, const char *body, size_t len)
{
    esp_http_client_config_t cfg = {
        .url    = url,
        .method = HTTP_METHOD_POST,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, (int)len);

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "POST %d", esp_http_client_get_status_code(client));
    } else {
        ESP_LOGE(TAG, "POST failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    return err;
}

const transport_driver_t wifi_transport = {
    .init     = wifi_init,
    .is_ready = wifi_is_ready,
    .post     = wifi_post,
};

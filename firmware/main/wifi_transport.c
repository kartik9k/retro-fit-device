#include <stdbool.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "tcpip_adapter.h"

#include "wifi_manager.h"
#include "transport.h"

static const char *TAG = "wifi_transport";

/*
 * TLS certificate verification — development vs production:
 *
 * cert_pem is NULL here so the ESP8266 RTOS SDK's mbedTLS defaults to
 * VERIFY_NONE: the connection is TLS-encrypted but the server certificate
 * is not authenticated. Acceptable for internal development (Mode 3).
 *
 * TODO (production — DEFERRED §11): embed the correct CA cert PEM here
 * and set cfg.cert_pem = CA_CERT_PEM to enable full chain verification.
 * The cert must be retrieved from the live server:
 *   openssl s_client -connect <host>:443 -showcerts 2>/dev/null | \
 *     openssl x509 -noout -text
 * Identify the root CA, download its PEM, paste it below, and reflash.
 */

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
        /* cert_pem intentionally NULL — VERIFY_NONE for development (see comment above) */
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

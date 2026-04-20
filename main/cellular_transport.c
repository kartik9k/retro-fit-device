#include <stdbool.h>

#include "esp_err.h"
#include "esp_log.h"

#include "transport.h"

/*
 * Cellular transport stub — SIM7080G (NB-IoT / LTE-M).
 *
 * Not yet implemented. See DEFERRED.md §1 and §2 for the full context,
 * prerequisites, and PWRKEY / UART wiring details before starting.
 *
 * GPIO assignments (from HARDWARE.md):
 *   UART0 RX  — GPIO13  (after UART0 pin-swap)
 *   UART0 TX  — GPIO15  (after UART0 pin-swap; 10 kΩ pull-down to GND required)
 *   PWRKEY    — GPIO12  (hold LOW ≥ 1 s to power on, ≥ 1.2 s to power off)
 *   nRESET    — GPIO14  (optional)
 */

static const char *TAG = "cellular_transport";

static esp_err_t cellular_init(void)
{
    ESP_LOGE(TAG, "cellular driver not implemented — see DEFERRED.md §1");
    return ESP_ERR_NOT_SUPPORTED;
}

static bool cellular_is_ready(void)
{
    return false;
}

static esp_err_t cellular_post(const char *url, const char *body, size_t len)
{
    (void)url;
    (void)body;
    (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}

const transport_driver_t cellular_transport = {
    .init     = cellular_init,
    .is_ready = cellular_is_ready,
    .post     = cellular_post,
};

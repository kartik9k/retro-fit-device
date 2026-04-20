#pragma once

#include <stdbool.h>
#include "esp_err.h"

/*
 * Transport-agnostic network interface.
 *
 * To add a new transport:
 *   1. Create <transport_name>_transport.c
 *   2. Implement init(), is_ready(), and post() with the signatures below
 *   3. Expose a const transport_driver_t for that driver
 *   4. Register it in transport_get() (main/transport.c)
 *
 * Current drivers:
 *   wifi_transport     — STA Wi-Fi via wifi_manager (main/wifi_transport.c)
 *   cellular_transport — SIM7080G stub; not yet implemented (main/cellular_transport.c)
 */

typedef struct {
    /*
     * One-time initialisation. Blocks until the transport is connected and
     * an IP address (or equivalent) is held. Returns ESP_OK on success.
     */
    esp_err_t (*init)(void);

    /*
     * Non-blocking connectivity check. Called from timer context — must
     * never block. Returns true when the transport is ready to POST.
     */
    bool (*is_ready)(void);

    /*
     * Send an HTTP POST with a pre-built JSON body. Blocking.
     * Returns ESP_OK on a successful HTTP exchange (any status code).
     */
    esp_err_t (*post)(const char *url, const char *body, size_t len);
} transport_driver_t;

/*
 * Returns the active transport driver selected at startup.
 * See DEFERRED.md §4 for the NVS-based runtime selection that replaces
 * the current compile-time default once the cellular driver is ready.
 */
const transport_driver_t *transport_get(void);

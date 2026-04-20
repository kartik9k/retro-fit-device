#include "transport.h"

/* Drivers implemented in their respective .c files */
extern const transport_driver_t wifi_transport;
extern const transport_driver_t cellular_transport;

const transport_driver_t *transport_get(void)
{
    /*
     * TODO (DEFERRED §4): read NVS key "transport/type" here and return
     * &cellular_transport when the value is "cellular".  NVS must be
     * initialised in app_main() before this call is made.  For now,
     * Wi-Fi is always selected.
     */
    return &wifi_transport;
}

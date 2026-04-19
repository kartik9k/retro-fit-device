#pragma once

#include "esp_err.h"

/*
 * Initialise NVS, then connect to Wi-Fi using stored credentials.
 *
 * Virgin device (no credentials in NVS)
 * --------------------------------------
 *   Opens a SoftAP named "retro-fit-XXXXXX" (last 3 MAC bytes, no password).
 *   Connect to that network and open http://192.168.4.1 in a browser.
 *   Fill in the SSID and password form; the device saves them to NVS and
 *   restarts automatically.  On the next boot this function connects directly.
 *
 * Normal boot (credentials present)
 * -----------------------------------
 *   Connects to the stored network and blocks until an IP address is obtained.
 *   Automatically reconnects on disconnection.
 *
 * Returns ESP_OK once an IP address is held.
 * Never returns ESP_OK during the provisioning boot (device restarts instead).
 */
esp_err_t wifi_manager_init(void);

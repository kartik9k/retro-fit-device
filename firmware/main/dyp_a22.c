#include "dyp_a22.h"

#include "esp_log.h"
#include "esp_err.h"
#include "distance_sensor.h"

/*
 * TODO — implement once hardware is received.
 *
 * Implementation checklist:
 *   1. Initialise I2C master on GPIO4 (SDA) / GPIO5 (SCL), 400 kHz.
 *   2. Probe the DYP-A22 at its default I2C address (confirm from datasheet
 *      — typical DYP I2C address is 0x11 but verify on received unit).
 *   3. In read_mm(): send measurement trigger command, wait for result
 *      register to be ready (poll or use data-ready signal if available),
 *      read 16-bit distance value, convert to mm.
 *   4. If multiple DYP-A22 sensors are ever placed on the same I2C bus,
 *      verify address configuration support with Hardware Agent first.
 *   5. Remove the ESP_LOGE "not implemented" log once driver is functional.
 *   6. Update HARDWARE.md change log and FIRMWARE.md sensor table on the
 *      same commit as the completed driver.
 *
 * Hardware prerequisities (HARDWARE.md § Hardware configurations v2):
 *   - HC-SR04 voltage divider (1 kΩ / 2 kΩ) removed from GPIO4
 *   - 4.7 kΩ pull-up resistors fitted on GPIO4 (SDA) and GPIO5 (SCL)
 *   - DYP-A22 configured for I2C output mode (hardware jumper or default)
 */

static const char *TAG = "dyp_a22";

static esp_err_t dyp_a22_init(void)
{
    ESP_LOGE(TAG, "DYP-A22 driver not yet implemented — hardware not received. "
                  "See firmware/main/dyp_a22.c TODO block.");
    return ESP_ERR_NOT_SUPPORTED;
}

static int32_t dyp_a22_read_mm(void)
{
    return DISTANCE_SENSOR_ERR;
}

const distance_sensor_t dyp_a22_sensor = {
    .init    = dyp_a22_init,
    .read_mm = dyp_a22_read_mm,
};

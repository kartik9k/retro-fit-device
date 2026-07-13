#include "dyp_a22.h"

#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "distance_sensor.h"

#define DYP_I2C_PORT        I2C_NUM_0
#define DYP_I2C_SDA         GPIO_NUM_4
#define DYP_I2C_SCL         GPIO_NUM_5
#define DYP_I2C_ADDR        0x74
#define DYP_REG_TRIGGER     0x10
#define DYP_CMD_TRIGGER     0xBD
#define DYP_REG_RESULT      0x02
#define DYP_MEASURE_MS      80      /* datasheet minimum before result is valid */
#define DYP_TIMEOUT_TICKS   (100 / portTICK_RATE_MS)

static const char *TAG = "dyp_a22";

static void i2c_scan(void)
{
    ESP_LOGI(TAG, "I2C scan start");
    bool found = false;
    for (uint8_t addr = 1; addr < 127; addr++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t err = i2c_master_cmd_begin(DYP_I2C_PORT, cmd, DYP_TIMEOUT_TICKS);
        i2c_cmd_link_delete(cmd);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "  found device at 0x%02x", addr);
            found = true;
        }
    }
    if (!found) ESP_LOGW(TAG, "  no devices found — check wiring");
    ESP_LOGI(TAG, "I2C scan done");
}

static esp_err_t dyp_a22_init(void)
{
    i2c_config_t cfg = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = DYP_I2C_SDA,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_io_num       = DYP_I2C_SCL,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .clk_stretch_tick = 300,    /* ~210 µs stretch; suits 100 kHz default */
    };
    esp_err_t err = i2c_driver_install(DYP_I2C_PORT, cfg.mode);
    if (err != ESP_OK) return err;
    err = i2c_param_config(DYP_I2C_PORT, &cfg);
    if (err != ESP_OK) return err;
    i2c_scan();
    return ESP_OK;
}

static int32_t dyp_a22_read_mm(void)
{
    /* Step 1: trigger a measurement — write 0xBD to register 0x10 */
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (DYP_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, DYP_REG_TRIGGER, true);
    i2c_master_write_byte(cmd, DYP_CMD_TRIGGER, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(DYP_I2C_PORT, cmd, DYP_TIMEOUT_TICKS);
    i2c_cmd_link_delete(cmd);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "trigger failed: %s", esp_err_to_name(err));
        return DISTANCE_SENSOR_ERR;
    }

    /* Step 2: wait for measurement to complete */
    vTaskDelay(pdMS_TO_TICKS(DYP_MEASURE_MS));

    /* Step 3: select result register 0x02, then read H and L bytes */
    cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (DYP_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, DYP_REG_RESULT, true);
    i2c_master_start(cmd);  /* repeated start */
    i2c_master_write_byte(cmd, (DYP_I2C_ADDR << 1) | I2C_MASTER_READ, true);
    uint8_t buf[2] = {0};
    i2c_master_read(cmd, buf, sizeof(buf), I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    err = i2c_master_cmd_begin(DYP_I2C_PORT, cmd, DYP_TIMEOUT_TICKS);
    i2c_cmd_link_delete(cmd);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "read failed: %s", esp_err_to_name(err));
        return DISTANCE_SENSOR_ERR;
    }

    uint16_t dist_mm = ((uint16_t)buf[0] << 8) | buf[1];
    if (dist_mm == 0 || dist_mm == 0xFFFF) return DISTANCE_SENSOR_ERR;
    return (int32_t)dist_mm;
}

const distance_sensor_t dyp_a22_sensor = {
    .init    = dyp_a22_init,
    .read_mm = dyp_a22_read_mm,
};

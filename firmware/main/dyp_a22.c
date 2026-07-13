#include "dyp_a22.h"

#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"

#include "distance_sensor.h"
#include "telemetry.h"

#define DYP_I2C_PORT            I2C_NUM_0
#define DYP_I2C_SDA             GPIO_NUM_4
#define DYP_I2C_SCL             GPIO_NUM_5
#define DYP_I2C_ADDR            0x74
#define DYP_REG_TRIGGER         0x10
#define DYP_CMD_TRIGGER         0xBD
#define DYP_REG_RESULT          0x02
#define DYP_MEASURE_MS          80          /* datasheet minimum before result is valid */
#define DYP_TIMEOUT_TICKS       (100 / portTICK_RATE_MS)
#define DYP_MUTEX_WAIT_TICKS    (50  / portTICK_RATE_MS)

/* Change this value to adjust the periodic I2C bus health-check interval */
#define DYP_HEALTH_SCAN_MS      (60UL * 60UL * 1000UL)     /* 1 hour */

static const char *TAG = "dyp_a22";

static SemaphoreHandle_t s_bus_mutex;    /* serialises every I2C transaction */
static SemaphoreHandle_t s_val_mutex;    /* protects s_last_mm */
static TimerHandle_t     s_meas_timer;   /* one-shot: fires DYP_MEASURE_MS after each trigger */
static TimerHandle_t     s_health_timer; /* periodic: I2C bus health scan */
static int32_t           s_last_mm = DISTANCE_SENSOR_ERR;

/* Caller must hold s_bus_mutex.
 * Returns true if DYP_I2C_ADDR (0x74) acknowledged — used by health_scan_cb. */
static bool i2c_scan(void)
{
    ESP_LOGI(TAG, "I2C scan");
    bool sensor_present = false;
    for (uint8_t addr = 1; addr < 127; addr++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t err = i2c_master_cmd_begin(DYP_I2C_PORT, cmd, DYP_TIMEOUT_TICKS);
        i2c_cmd_link_delete(cmd);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "  found 0x%02x", addr);
            if (addr == DYP_I2C_ADDR) sensor_present = true;
        }
    }
    if (!sensor_present) ESP_LOGW(TAG, "  DYP-A22 (0x74) not found — check wiring");
    return sensor_present;
}

/* Send a measurement trigger and arm s_meas_timer.
 * Acquires and releases s_bus_mutex internally. */
static esp_err_t dyp_trigger(void)
{
    if (xSemaphoreTake(s_bus_mutex, DYP_MUTEX_WAIT_TICKS) != pdTRUE) {
        ESP_LOGW(TAG, "trigger: bus busy");
        return ESP_ERR_TIMEOUT;
    }
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (DYP_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, DYP_REG_TRIGGER, true);
    i2c_master_write_byte(cmd, DYP_CMD_TRIGGER, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(DYP_I2C_PORT, cmd, DYP_TIMEOUT_TICKS);
    i2c_cmd_link_delete(cmd);
    xSemaphoreGive(s_bus_mutex);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "trigger failed: %s", esp_err_to_name(err));
        return err;
    }
    xTimerStart(s_meas_timer, 0);
    return ESP_OK;
}

/* Fires DYP_MEASURE_MS after each trigger; runs in the FreeRTOS timer-daemon task.
 * Reads the completed measurement into s_last_mm, then fires the next trigger
 * to keep the pipeline self-sustaining. */
static void meas_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;

    if (xSemaphoreTake(s_bus_mutex, DYP_MUTEX_WAIT_TICKS) != pdTRUE) {
        ESP_LOGW(TAG, "read: bus busy");
        dyp_trigger();
        return;
    }

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (DYP_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, DYP_REG_RESULT, true);
    i2c_master_start(cmd);  /* repeated start */
    i2c_master_write_byte(cmd, (DYP_I2C_ADDR << 1) | I2C_MASTER_READ, true);
    uint8_t buf[2] = {0};
    i2c_master_read(cmd, buf, sizeof(buf), I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(DYP_I2C_PORT, cmd, DYP_TIMEOUT_TICKS);
    i2c_cmd_link_delete(cmd);
    xSemaphoreGive(s_bus_mutex);

    xSemaphoreTake(s_val_mutex, portMAX_DELAY);
    if (err == ESP_OK) {
        uint16_t dist_mm = ((uint16_t)buf[0] << 8) | buf[1];
        s_last_mm = (dist_mm == 0 || dist_mm == 0xFFFF)
                    ? DISTANCE_SENSOR_ERR
                    : (int32_t)dist_mm;
    } else {
        ESP_LOGE(TAG, "read failed: %s", esp_err_to_name(err));
        s_last_mm = DISTANCE_SENSOR_ERR;
    }
    xSemaphoreGive(s_val_mutex);

    dyp_trigger();  /* pipeline: start the next measurement immediately */
}

/* Periodic health check — skips gracefully if the bus is in use.
 * Reports sensor presence via i2c_health event so the server can track connectivity. */
static void health_scan_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    if (xSemaphoreTake(s_bus_mutex, DYP_MUTEX_WAIT_TICKS) != pdTRUE) {
        ESP_LOGW(TAG, "health scan: bus busy, skipping");
        return;
    }
    bool found = i2c_scan();
    xSemaphoreGive(s_bus_mutex);

    uint32_t ts = (uint32_t)(esp_timer_get_time() / 1000);
    telemetry_push_event("i2c_health", TELEM_BOOL(found), ts);
}

static esp_err_t dyp_a22_init(void)
{
    s_bus_mutex = xSemaphoreCreateMutex();
    s_val_mutex = xSemaphoreCreateMutex();
    if (!s_bus_mutex || !s_val_mutex) return ESP_ERR_NO_MEM;

    i2c_config_t cfg = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = DYP_I2C_SDA,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_io_num       = DYP_I2C_SCL,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .clk_stretch_tick = 300,
    };
    esp_err_t err = i2c_driver_install(DYP_I2C_PORT, cfg.mode);
    if (err != ESP_OK) return err;
    err = i2c_param_config(DYP_I2C_PORT, &cfg);
    if (err != ESP_OK) return err;

    xSemaphoreTake(s_bus_mutex, portMAX_DELAY);
    i2c_scan();
    xSemaphoreGive(s_bus_mutex);

    s_meas_timer = xTimerCreate("dyp_meas",
                                pdMS_TO_TICKS(DYP_MEASURE_MS),
                                pdFALSE, NULL, meas_timer_cb);
    s_health_timer = xTimerCreate("dyp_health",
                                  pdMS_TO_TICKS(DYP_HEALTH_SCAN_MS),
                                  pdTRUE, NULL, health_scan_cb);
    if (!s_meas_timer || !s_health_timer) return ESP_ERR_NO_MEM;

    xTimerStart(s_health_timer, 0);

    err = dyp_trigger();    /* prime the pipeline */
    return (err == ESP_OK) ? ESP_OK : ESP_FAIL;
}

/* Returns the most recent measurement from the continuous pipeline.
 * Always returns immediately — relies on the pipeline running faster than
 * the application sample rate (82 ms/cycle vs SENSOR_PERIOD_MS). */
static int32_t dyp_a22_read_mm(void)
{
    xSemaphoreTake(s_val_mutex, portMAX_DELAY);
    int32_t mm = s_last_mm;
    xSemaphoreGive(s_val_mutex);
    return mm;
}

const distance_sensor_t dyp_a22_sensor = {
    .init    = dyp_a22_init,
    .read_mm = dyp_a22_read_mm,
};

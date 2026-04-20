#include "hcsr04.h"

#include "driver/gpio.h"
#include "rom/ets_sys.h"
#include "esp_timer.h"
#include "esp_err.h"

#define TRIG_GPIO       GPIO_NUM_5
#define ECHO_GPIO       GPIO_NUM_4
#define TIMEOUT_US      38000   /* datasheet max echo pulse; ~6.5 m range */

static esp_err_t hcsr04_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << TRIG_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) return err;
    gpio_set_level(TRIG_GPIO, 0);

    io.pin_bit_mask = (1ULL << ECHO_GPIO);
    io.mode         = GPIO_MODE_INPUT;
    return gpio_config(&io);
}

static int32_t hcsr04_read_mm(void)
{
    /* 10 µs trigger pulse */
    gpio_set_level(TRIG_GPIO, 1);
    ets_delay_us(10);
    gpio_set_level(TRIG_GPIO, 0);

    /* Wait for echo HIGH */
    int64_t t0 = esp_timer_get_time();
    while (gpio_get_level(ECHO_GPIO) == 0) {
        if ((esp_timer_get_time() - t0) > TIMEOUT_US)
            return DISTANCE_SENSOR_ERR;
    }
    int64_t echo_start = esp_timer_get_time();

    /* Wait for echo LOW */
    while (gpio_get_level(ECHO_GPIO) == 1) {
        if ((esp_timer_get_time() - echo_start) > TIMEOUT_US)
            return DISTANCE_SENSOR_ERR;
    }
    int64_t duration_us = esp_timer_get_time() - echo_start;

    /* distance_mm = duration_us × 0.3432 / 2  ≈  duration_us × 3432 / 20000 */
    return (int32_t)(duration_us * 3432 / 20000);
}

const distance_sensor_t hcsr04_sensor = {
    .init    = hcsr04_init,
    .read_mm = hcsr04_read_mm,
};

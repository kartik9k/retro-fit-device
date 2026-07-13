#include "telemetry.h"

#include <string.h>
#include <inttypes.h>

#include "esp_log.h"
#include "freertos/semphr.h"

static const char *TAG = "telemetry";

/* -------------------------------------------------------------------------- */
/* Internal types                                                              */
/* -------------------------------------------------------------------------- */

typedef struct {
    const char *key;          /* SENSOR_TYPE_TAG — always a string literal */
    int32_t     mm;           /* TELEM_READING_ERR encodes a null reading   */
    uint32_t    timestamp_ms;
} telem_reading_t;

typedef struct {
    const char  *key;
    telem_val_t  val;
    uint32_t     timestamp_ms;
} telem_event_t;

/* -------------------------------------------------------------------------- */
/* State                                                                       */
/* -------------------------------------------------------------------------- */

static telem_reading_t   s_readings[READING_QUEUE_DEPTH];
static telem_event_t     s_events[EVENT_QUEUE_DEPTH];
static int               s_reading_count;
static int               s_event_count;
static SemaphoreHandle_t s_mutex;
static TaskHandle_t      s_post_task;
static const char       *s_device_id;

/* -------------------------------------------------------------------------- */
/* Init                                                                        */
/* -------------------------------------------------------------------------- */

esp_err_t telemetry_init(TaskHandle_t post_task_handle, const char *device_id)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;
    s_post_task = post_task_handle;
    s_device_id = device_id;
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* Push                                                                        */
/* -------------------------------------------------------------------------- */

esp_err_t telemetry_push_reading(const char *key, int32_t mm, uint32_t timestamp_ms)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_reading_count == READING_QUEUE_DEPTH) {
        /* Evict oldest to make room for the newest */
        memmove(s_readings, s_readings + 1,
                (READING_QUEUE_DEPTH - 1) * sizeof(telem_reading_t));
        s_reading_count--;
    }
    s_readings[s_reading_count++] = (telem_reading_t){
        .key = key, .mm = mm, .timestamp_ms = timestamp_ms,
    };
    bool flush = (s_reading_count >= READING_FLUSH_AT);
    xSemaphoreGive(s_mutex);

    if (flush) xTaskNotifyGive(s_post_task);
    return ESP_OK;
}

esp_err_t telemetry_push_event(const char *key, telem_val_t val, uint32_t timestamp_ms)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_event_count == EVENT_QUEUE_DEPTH) {
        ESP_LOGW(TAG, "event buffer full — evicting oldest (%s)", s_events[0].key);
        memmove(s_events, s_events + 1,
                (EVENT_QUEUE_DEPTH - 1) * sizeof(telem_event_t));
        s_event_count--;
    }
    s_events[s_event_count++] = (telem_event_t){
        .key = key, .val = val, .timestamp_ms = timestamp_ms,
    };
    bool flush = (s_event_count >= EVENT_FLUSH_AT);
    xSemaphoreGive(s_mutex);

    if (flush) xTaskNotifyGive(s_post_task);
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* Build POST body                                                             */
/* -------------------------------------------------------------------------- */

int telemetry_build_post(char *buf, size_t len)
{
    /* Snapshot both buffers and clear them under mutex so producers can
     * continue pushing immediately — JSON building happens outside the lock */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    telem_reading_t readings[READING_QUEUE_DEPTH];
    telem_event_t   events[EVENT_QUEUE_DEPTH];
    int nr = s_reading_count;
    int ne = s_event_count;
    memcpy(readings, s_readings, (size_t)nr * sizeof(telem_reading_t));
    memcpy(events,   s_events,   (size_t)ne * sizeof(telem_event_t));
    s_reading_count = 0;
    s_event_count   = 0;
    xSemaphoreGive(s_mutex);

    if (nr == 0 && ne == 0) {
        buf[0] = '\0';
        return 0;
    }

    int pos = snprintf(buf, len,
                       "{\"proto\":\"%s\",\"device\":\"%s\",\"readings\":[",
                       PROTO_VERSION, s_device_id);

    for (int i = 0; i < nr; i++) {
        const telem_reading_t *r = &readings[i];
        /* 32-byte reserve: enough for "]}" or "],"events":[" closing skeleton */
        int room = (int)len - pos - 32;
        if (room <= 0) {
            ESP_LOGW(TAG, "truncated at reading %d/%d", i, nr);
            break;
        }
        if (r->mm == TELEM_READING_ERR) {
            pos += snprintf(buf + pos, (size_t)room,
                            "{\"k\":\"%s\",\"v\":null,\"t\":%"PRIu32"}",
                            r->key, r->timestamp_ms);
        } else {
            pos += snprintf(buf + pos, (size_t)room,
                            "{\"k\":\"%s\",\"v\":%"PRId32".%"PRId32",\"t\":%"PRIu32"}",
                            r->key,
                            r->mm / 10, r->mm % 10,
                            r->timestamp_ms);
        }
        if (i < nr - 1) buf[pos++] = ',';
    }

    if (ne > 0) {
        pos += snprintf(buf + pos, len - (size_t)pos, "],\"events\":[");
        for (int i = 0; i < ne; i++) {
            const telem_event_t *e = &events[i];
            int room = (int)len - pos - 4; /* reserve for "]}" + null */
            if (room <= 0) {
                ESP_LOGW(TAG, "truncated at event %d/%d", i, ne);
                break;
            }
            int written = 0;
            switch (e->val.type) {
                case TELEM_BOOL:
                    written = snprintf(buf + pos, (size_t)room,
                                       "{\"k\":\"%s\",\"v\":%s,\"t\":%"PRIu32"}",
                                       e->key,
                                       e->val.b ? "true" : "false",
                                       e->timestamp_ms);
                    break;
                case TELEM_INT:
                    written = snprintf(buf + pos, (size_t)room,
                                       "{\"k\":\"%s\",\"v\":%"PRId32",\"t\":%"PRIu32"}",
                                       e->key, e->val.i, e->timestamp_ms);
                    break;
                case TELEM_STR:
                    written = snprintf(buf + pos, (size_t)room,
                                       "{\"k\":\"%s\",\"v\":\"%.*s\",\"t\":%"PRIu32"}",
                                       e->key,
                                       (int)e->val.s.len, e->val.s.ptr,
                                       e->timestamp_ms);
                    break;
            }
            pos += written;
            if (i < ne - 1) buf[pos++] = ',';
        }
    }

    snprintf(buf + pos, len - (size_t)pos, "]}");
    return nr + ne;
}

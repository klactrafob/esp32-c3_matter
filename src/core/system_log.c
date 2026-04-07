#include "core/system_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define SYSTEM_LOG_CAPACITY 48

typedef struct {
    bool used;
    int64_t ts_ms;
    char source[16];
    char level[8];
    char message[128];
} system_log_entry_t;

static SemaphoreHandle_t s_lock = NULL;
static system_log_entry_t s_entries[SYSTEM_LOG_CAPACITY];
static int s_write_index = 0;
static int s_count = 0;

static void ensure_lock(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
}

void system_log_init(void)
{
    ensure_lock();
}

void system_log_write(const char *source, const char *level, const char *message)
{
    ensure_lock();
    if (!s_lock) {
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    system_log_entry_t *entry = &s_entries[s_write_index];
    memset(entry, 0, sizeof(*entry));
    entry->used = true;
    entry->ts_ms = esp_timer_get_time() / 1000LL;
    snprintf(entry->source, sizeof(entry->source), "%s", source ? source : "sys");
    snprintf(entry->level, sizeof(entry->level), "%s", level ? level : "info");
    snprintf(entry->message, sizeof(entry->message), "%s", message ? message : "");
    s_write_index = (s_write_index + 1) % SYSTEM_LOG_CAPACITY;
    if (s_count < SYSTEM_LOG_CAPACITY) {
        s_count++;
    }
    xSemaphoreGive(s_lock);
}

void system_log_writef(const char *source, const char *level, const char *fmt, ...)
{
    char message[128];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);

    system_log_write(source, level, message);
}

cJSON *system_log_build_json(int max_items)
{
    cJSON *items = cJSON_CreateArray();
    if (!items) {
        return NULL;
    }

    ensure_lock();
    if (!s_lock) {
        return items;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    int count = s_count;
    if (max_items > 0 && count > max_items) {
        count = max_items;
    }

    int start = (s_write_index - count + SYSTEM_LOG_CAPACITY) % SYSTEM_LOG_CAPACITY;
    for (int i = 0; i < count; ++i) {
        const system_log_entry_t *entry = &s_entries[(start + i) % SYSTEM_LOG_CAPACITY];
        if (!entry->used) {
            continue;
        }

        cJSON *obj = cJSON_CreateObject();
        if (!obj) {
            continue;
        }
        cJSON_AddNumberToObject(obj, "ts_ms", (double)entry->ts_ms);
        cJSON_AddStringToObject(obj, "source", entry->source);
        cJSON_AddStringToObject(obj, "level", entry->level);
        cJSON_AddStringToObject(obj, "message", entry->message);
        cJSON_AddItemToArray(items, obj);
    }

    xSemaphoreGive(s_lock);
    return items;
}

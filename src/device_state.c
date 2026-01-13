#include "device_state.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static device_state_t s_state;
static SemaphoreHandle_t s_mtx;

void device_state_init(void)
{
    s_mtx = xSemaphoreCreateMutex();
    s_state.relay_on = false;
    s_state.level = 0;
}

device_state_t device_state_get(void)
{
    device_state_t out;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    out = s_state;
    xSemaphoreGive(s_mtx);
    return out;
}

void device_state_set_relay(bool on)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_state.relay_on = on;
    xSemaphoreGive(s_mtx);
}

void device_state_set_level(uint8_t level)
{
    if (level > 100) level = 100;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_state.level = level;
    xSemaphoreGive(s_mtx);
}

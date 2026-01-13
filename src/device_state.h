#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool relay_on;
    uint8_t level; // 0..100
} device_state_t;

void device_state_init(void);

device_state_t device_state_get(void);
void device_state_set_relay(bool on);
void device_state_set_level(uint8_t level);

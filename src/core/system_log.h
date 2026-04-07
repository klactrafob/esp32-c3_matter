#pragma once

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

void system_log_init(void);
void system_log_write(const char *source, const char *level, const char *message);
void system_log_writef(const char *source, const char *level, const char *fmt, ...);
cJSON *system_log_build_json(int max_items);

#ifdef __cplusplus
}
#endif

#ifndef CONTROL_WIGGLE_H
#define CONTROL_WIGGLE_H

#include <stdbool.h>
#include <stdint.h>
#include "app/context.h"

int wiggle_start(app_context_t *app,
                 uint32_t id,
                 uint16_t start_value,
                 uint16_t end_value,
                 uint16_t exit_value,
                 uint32_t period_ms,
                 uint32_t total_duration_ms);
bool wiggle_stop(app_context_t *app, uint32_t id);
void wiggle_stop_all(app_context_t *app, bool send_exit_value);
void wiggle_process(app_context_t *app);

#endif

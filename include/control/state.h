#ifndef CONTROL_STATE_H
#define CONTROL_STATE_H

#include <stdbool.h>
#include "app/context.h"

const char *app_state_label(app_state_t state);
app_state_t app_state_get(app_context_t *app);
bool app_state_set(app_context_t *app, app_state_t state);
void app_state_broadcast(app_context_t *app);

#endif

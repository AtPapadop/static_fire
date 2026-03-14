#ifndef WS_WS_BRIDGE_H
#define WS_WS_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <simple_ws/simple_ws.h>
#include "app/context.h"

int ws_bridge_start(app_context_t *app);
void ws_bridge_stop(app_context_t *app);

void ws_bridge_broadcast_text(app_context_t *app, const char *msg, bool prefix_timestamp);
void ws_bridge_send_text(app_context_t *app, ws_client_t *client, const char *msg);
void ws_bridge_send_current_state(app_context_t *app, ws_client_t *client);

#endif

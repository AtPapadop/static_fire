#ifndef CONTROL_COMMAND_H
#define CONTROL_COMMAND_H

#include "app/context.h"

struct ws_client;

void command_handle_text(app_context_t *app, struct ws_client *client, const char *text);

#endif

#ifndef CONTROL_FIRING_SEQUENCE_H
#define CONTROL_FIRING_SEQUENCE_H

#include <stdbool.h>
#include <stdint.h>
#include "app/context.h"

struct ws_client;

int firing_sequence_parse(const char *command, firing_sequence_config_t *config);
int firing_sequence_start(app_context_t *app,
                          struct ws_client *client,
                          const firing_sequence_config_t *config,
                          const char *started_message,
                          const char *completed_message,
                          const char *failed_message);
int firing_sequence_start_drown(app_context_t *app, struct ws_client *client);
void firing_sequence_request_abort(app_context_t *app);

#endif

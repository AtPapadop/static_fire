#define _POSIX_C_SOURCE 200809L

#include "control/firing_sequence.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "can/can_bus.h"
#include "util/logger.h"
#include "ws/ws_bridge.h"

#define FIRING_SEQUENCE_TMINUS_MS 10000u
#define CAN_SEND_ATTEMPTS 3

typedef struct
{
	app_context_t *app;
	ws_client_t *client;
	firing_sequence_config_t config;
	char started_message[160];
	char completed_message[128];
	char failed_message[128];
} sequence_worker_args_t;

static bool abort_requested(app_context_t *app)
{
	bool requested = false;
	pthread_mutex_lock(&app->sequence_lock);
	requested = app->sequence_abort;
	pthread_mutex_unlock(&app->sequence_lock);
	return requested;
}

static void finish_sequence(app_context_t *app)
{
	pthread_mutex_lock(&app->sequence_lock);
	app->sequence_active = false;
	app->sequence_abort = false;
	pthread_mutex_unlock(&app->sequence_lock);
}

static bool wait_with_abort(app_context_t *app, uint32_t delay_ms)
{
	uint64_t deadline = monotonic_ms() + delay_ms;
	while (monotonic_ms() < deadline)
	{
		if (abort_requested(app) || app->stop)
		{
			return false;
		}
		struct timespec ts = {.tv_sec = 0, .tv_nsec = 10 * 1000 * 1000};
		nanosleep(&ts, NULL);
	}
	return true;
}

static int send_block(app_context_t *app, uint32_t id, uint16_t value, uint16_t duration_ms)
{
	for (int i = 0; i < CAN_SEND_ATTEMPTS; ++i)
	{
		if (can_bus_send_command(app->can_fd, id, value, duration_ms) == 0)
		{
			logger_printf(&app->logger, "CAN_TX", "%03" PRIx32 "#%04" PRIx16 "#%u", id, value, duration_ms);
			return 0;
		}
	}
	return -1;
}

static void *sequence_worker(void *arg)
{
	sequence_worker_args_t *args = (sequence_worker_args_t *)arg;
	app_context_t *app = args->app;

	ws_bridge_send_text(app, args->client, args->started_message);

	uint32_t pre_delay_ms = args->config.drown_injector ? 0u : FIRING_SEQUENCE_TMINUS_MS;

	for (uint8_t i = 0; i < args->config.num_blocks; ++i)
	{
		uint32_t delay_ms = (i == 0) ? pre_delay_ms : args->config.wait_times_ms[i - 1];
		if (!wait_with_abort(app, delay_ms))
		{
			ws_bridge_send_text(app, args->client, "Firing sequence aborted by command or shutdown");
			finish_sequence(app);
			free(args);
			return NULL;
		}

		if (send_block(app,
									 args->config.ids[i],
									 args->config.values[i][0],
									 args->config.values[i][1]) != 0)
		{
			ws_bridge_send_text(app, args->client, args->failed_message);
			finish_sequence(app);
			free(args);
			return NULL;
		}
	}

	ws_bridge_send_text(app, args->client, args->completed_message);
	finish_sequence(app);
	free(args);
	return NULL;
}

int firing_sequence_parse(const char *command, firing_sequence_config_t *config)
{
	if (!command || !config)
	{
		return -1;
	}

	memset(config, 0, sizeof(*config));

	const char *payload = strchr(command, '|');
	if (!payload)
	{
		return -1;
	}
	++payload;

	char *copy = strdup(payload);
	if (!copy)
	{
		return -1;
	}

	char *saveptr = NULL;
	char *token = strtok_r(copy, "|", &saveptr);
	while (token && config->num_blocks < APP_MAX_FIRING_BLOCKS)
	{
		uint32_t id = 0;
		uint16_t value = 0;
		uint16_t duration_ms = 0;
		uint16_t wait_ms = 0;
		char extra = '\0';

		int parsed = sscanf(token,
												"%" SCNx32 "#%" SCNx16 "#%" SCNu16 "#%" SCNu16 "%c",
												&id,
												&value,
												&duration_ms,
												&wait_ms,
												&extra);

		if (parsed == 4)
		{
			config->ids[config->num_blocks] = id;
			config->values[config->num_blocks][0] = value;
			config->values[config->num_blocks][1] = duration_ms;
			config->wait_times_ms[config->num_blocks] = wait_ms;
		}
		else
		{
			parsed = sscanf(token,
											"%" SCNx32 "#%" SCNx16 "#%" SCNu16 "%c",
											&id,
											&value,
											&duration_ms,
											&extra);
			if (parsed != 3)
			{
				free(copy);
				return -1;
			}
			config->ids[config->num_blocks] = id;
			config->values[config->num_blocks][0] = value;
			config->values[config->num_blocks][1] = duration_ms;
			config->wait_times_ms[config->num_blocks] = 0;
		}

		if (config->ids[config->num_blocks] == 0 || config->ids[config->num_blocks] > 0x7FFu)
		{
			free(copy);
			return -1;
		}

		++config->num_blocks;
		token = strtok_r(NULL, "|", &saveptr);
	}

	free(copy);
	return config->num_blocks > 0 ? 0 : -1;
}

int firing_sequence_start(app_context_t *app,
													ws_client_t *client,
													const firing_sequence_config_t *config,
													const char *started_message,
													const char *completed_message,
													const char *failed_message)
{
	if (!app || !client || !config || config->num_blocks == 0)
	{
		return -1;
	}

	pthread_mutex_lock(&app->sequence_lock);
	if (app->sequence_active)
	{
		pthread_mutex_unlock(&app->sequence_lock);
		return -1;
	}
	app->sequence_active = true;
	app->sequence_abort = false;
	pthread_mutex_unlock(&app->sequence_lock);

	sequence_worker_args_t *args = calloc(1, sizeof(*args));
	if (!args)
	{
		finish_sequence(app);
		return -1;
	}

	args->app = app;
	args->client = client;
	args->config = *config;
	snprintf(args->started_message, sizeof(args->started_message), "%s", started_message ? started_message : "Firing sequence started");
	snprintf(args->completed_message, sizeof(args->completed_message), "%s", completed_message ? completed_message : "Firing sequence completed");
	snprintf(args->failed_message, sizeof(args->failed_message), "%s", failed_message ? failed_message : "Firing sequence failed to start");

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	int rc = pthread_create(&app->sequence_thread, &attr, sequence_worker, args);
	pthread_attr_destroy(&attr);
	if (rc != 0)
	{
		finish_sequence(app);
		free(args);
		errno = rc;
		return -1;
	}

	return 0;
}

int firing_sequence_start_drown(app_context_t *app, ws_client_t *client)
{
	if (!app || !client)
	{
		return -1;
	}

	if (app->valve_profile.lox_main_valve_id == 0 || app->valve_profile.lox_vent_valve_id == 0)
	{
		return -1;
	}

	firing_sequence_config_t config;
	memset(&config, 0, sizeof(config));
	config.drown_injector = true;
	config.num_blocks = 2;
	config.ids[0] = app->valve_profile.lox_main_valve_id;
	config.values[0][0] = app->valve_profile.lox_main_angle_open;
	config.values[0][1] = 0;
	config.wait_times_ms[0] = 100;
	config.ids[1] = app->valve_profile.lox_vent_valve_id;
	config.values[1][0] = app->valve_profile.lox_vent_angle_close;
	config.values[1][1] = 1000;
	config.wait_times_ms[1] = 0;

	char started[160];
	snprintf(started,
					 sizeof(started),
					 "Starting drowning sequence: open 0x%03" PRIx32 " to 0x%04" PRIx16 ", then close 0x%03" PRIx32 " to 0x%04" PRIx16,
					 config.ids[0],
					 config.values[0][0],
					 config.ids[1],
					 config.values[1][0]);

	return firing_sequence_start(app,
															 client,
															 &config,
															 started,
															 "Injector drowned successfully",
															 "Error drowning injector");
}

void firing_sequence_request_abort(app_context_t *app)
{
	if (!app)
	{
		return;
	}

	pthread_mutex_lock(&app->sequence_lock);
	app->sequence_abort = true;
	pthread_mutex_unlock(&app->sequence_lock);
}

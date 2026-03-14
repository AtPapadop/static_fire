#ifndef APP_CONTEXT_H
#define APP_CONTEXT_H

#include <stdbool.h>
#include <signal.h>
#include <stdint.h>
#include <pthread.h>
#include <simple_ws/simple_ws.h>

#include "app/config.h"
#include "util/logger.h"

typedef enum
{
	APP_STATE_PRECHILLING = 0,
	APP_STATE_HOTFIRE = 1
} app_state_t;

typedef struct app_client_ctx
{
	struct ws_client *client;
	bool write_only;
	struct app_client_ctx *next;
} app_client_ctx_t;

typedef struct
{
	uint32_t id;
	uint16_t start_value;
	uint16_t end_value;
	uint16_t current_value;
	uint16_t exit_value;
	uint32_t period_ms;
	uint32_t total_duration_ms;
	uint64_t next_toggle_ms;
	uint64_t stop_time_ms;
	bool active;
} wiggle_command_t;

#define APP_MAX_WIGGLES 16
#define APP_MAX_FIRING_BLOCKS 24
#define APP_CAN_ID_COUNT 0x800

typedef struct
{
	uint8_t num_blocks;
	uint32_t ids[APP_MAX_FIRING_BLOCKS];
	uint16_t values[APP_MAX_FIRING_BLOCKS][2];
	uint16_t wait_times_ms[APP_MAX_FIRING_BLOCKS];
	bool drown_injector;
} firing_sequence_config_t;

typedef struct
{
	uint32_t lox_main_valve_id;
	uint16_t lox_main_angle_open;
	uint32_t lox_vent_valve_id;
	uint16_t lox_vent_angle_close;
} valve_profile_t;

typedef struct app_context
{
	app_config_t config;
	volatile sig_atomic_t stop;

	int epoll_fd;
	int timer_fd;
	int can_fd;

	ws_server_t *ws;

	pthread_mutex_t clients_lock;
	app_client_ctx_t *clients;

	pthread_mutex_t state_lock;
	app_state_t state;
	wiggle_command_t wiggles[APP_MAX_WIGGLES];

	pthread_mutex_t can_lock;
	int32_t can_values[APP_CAN_ID_COUNT];
	bool can_dirty[APP_CAN_ID_COUNT];

	pthread_mutex_t sequence_lock;
	bool sequence_active;
	bool sequence_abort;
	pthread_t sequence_thread;

	valve_profile_t valve_profile;
	logger_t logger;
} app_context_t;

void app_context_init(app_context_t *app, const app_config_t *cfg);
void app_context_cleanup(app_context_t *app);

#endif

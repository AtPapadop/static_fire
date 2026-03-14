#include "app/context.h"

#include <string.h>

void app_context_init(app_context_t *app, const app_config_t *cfg)
{
	if (!app || !cfg)
	{
		return;
	}

	memset(app, 0, sizeof(*app));
	app->config = *cfg;
	app->epoll_fd = -1;
	app->timer_fd = -1;
	app->can_fd = -1;
	app->state = APP_STATE_PRECHILLING;

	for (int i = 0; i < APP_CAN_ID_COUNT; ++i)
	{
		app->can_values[i] = -1;
		app->can_dirty[i] = false;
	}

	app->valve_profile.lox_main_valve_id = 0x709u;
	app->valve_profile.lox_main_angle_open = 0x0099u;
	app->valve_profile.lox_vent_valve_id = 0x702u;
	app->valve_profile.lox_vent_angle_close = 0x0074u;

	pthread_mutex_init(&app->clients_lock, NULL);
	pthread_mutex_init(&app->state_lock, NULL);
	pthread_mutex_init(&app->can_lock, NULL);
	pthread_mutex_init(&app->sequence_lock, NULL);
}

void app_context_cleanup(app_context_t *app)
{
	if (!app)
	{
		return;
	}

	pthread_mutex_destroy(&app->clients_lock);
	pthread_mutex_destroy(&app->state_lock);
	pthread_mutex_destroy(&app->can_lock);
	pthread_mutex_destroy(&app->sequence_lock);
}

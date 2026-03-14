#include "control/state.h"

#include <stdio.h>

#include "control/wiggle.h"
#include "ws/ws_bridge.h"

const char *app_state_label(app_state_t state)
{
	return (state == APP_STATE_PRECHILLING) ? "PRECHILLING" : "HOTFIRE";
}

app_state_t app_state_get(app_context_t *app)
{
	if (!app)
	{
		return APP_STATE_PRECHILLING;
	}

	pthread_mutex_lock(&app->state_lock);
	app_state_t state = app->state;
	pthread_mutex_unlock(&app->state_lock);
	return state;
}

bool app_state_set(app_context_t *app, app_state_t state)
{
	if (!app)
	{
		return false;
	}

	pthread_mutex_lock(&app->state_lock);
	bool changed = (app->state != state);
	app->state = state;
	pthread_mutex_unlock(&app->state_lock);

	if (state == APP_STATE_HOTFIRE)
	{
		wiggle_stop_all(app, true);
	}

	app_state_broadcast(app);
	return changed;
}

void app_state_broadcast(app_context_t *app)
{
	if (!app)
	{
		return;
	}

	char msg[64];
	snprintf(msg, sizeof(msg), "State has been set to %s", app_state_label(app_state_get(app)));
	ws_bridge_broadcast_text(app, msg, false);
}

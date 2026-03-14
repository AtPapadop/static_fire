#include "control/wiggle.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "can/can_bus.h"
#include "control/state.h"
#include "util/logger.h"
#include "ws/ws_bridge.h"

static wiggle_command_t *find_slot(app_context_t *app, uint32_t id)
{
	for (size_t i = 0; i < APP_MAX_WIGGLES; ++i)
	{
		if (app->wiggles[i].active && app->wiggles[i].id == id)
		{
			return &app->wiggles[i];
		}
	}
	return NULL;
}

static wiggle_command_t *alloc_slot(app_context_t *app)
{
	for (size_t i = 0; i < APP_MAX_WIGGLES; ++i)
	{
		if (!app->wiggles[i].active)
		{
			return &app->wiggles[i];
		}
	}
	return NULL;
}

static void send_value(app_context_t *app, wiggle_command_t *slot, uint16_t value)
{
	if (!app || !slot)
	{
		return;
	}

	slot->current_value = value;
	if (can_bus_send_command(app->can_fd, slot->id, value, 0) == 0)
	{
		logger_printf(&app->logger, "CAN_TX", "%03" PRIx32 "#%04" PRIx16, slot->id, value);
	}
}

static void deactivate_slot(app_context_t *app, wiggle_command_t *slot, bool send_exit_value)
{
	if (!app || !slot || !slot->active)
	{
		return;
	}

	if (send_exit_value)
	{
		send_value(app, slot, slot->exit_value);
	}
	memset(slot, 0, sizeof(*slot));
}

int wiggle_start(app_context_t *app,
								 uint32_t id,
								 uint16_t start_value,
								 uint16_t end_value,
								 uint16_t exit_value,
								 uint32_t period_ms,
								 uint32_t total_duration_ms)
{
	if (!app || period_ms == 0 || id > 0x7FFu)
	{
		return -1;
	}

	pthread_mutex_lock(&app->state_lock);
	wiggle_command_t *slot = find_slot(app, id);
	if (!slot)
	{
		slot = alloc_slot(app);
	}
	if (!slot)
	{
		pthread_mutex_unlock(&app->state_lock);
		return -1;
	}

	uint64_t now = monotonic_ms();
	slot->id = id;
	slot->start_value = start_value;
	slot->end_value = end_value;
	slot->exit_value = exit_value;
	slot->period_ms = period_ms;
	slot->total_duration_ms = total_duration_ms;
	slot->active = true;
	slot->stop_time_ms = total_duration_ms ? now + total_duration_ms : 0;
	send_value(app, slot, start_value);
	slot->next_toggle_ms = now + period_ms;
	pthread_mutex_unlock(&app->state_lock);
	return 0;
}

bool wiggle_stop(app_context_t *app, uint32_t id)
{
	if (!app)
	{
		return false;
	}

	pthread_mutex_lock(&app->state_lock);
	wiggle_command_t *slot = find_slot(app, id);
	bool stopped = slot != NULL;
	if (slot)
	{
		deactivate_slot(app, slot, true);
	}
	pthread_mutex_unlock(&app->state_lock);
	return stopped;
}

void wiggle_stop_all(app_context_t *app, bool send_exit_value)
{
	if (!app)
	{
		return;
	}

	pthread_mutex_lock(&app->state_lock);
	unsigned int stopped = 0;
	for (size_t i = 0; i < APP_MAX_WIGGLES; ++i)
	{
		if (app->wiggles[i].active)
		{
			deactivate_slot(app, &app->wiggles[i], send_exit_value);
			++stopped;
		}
	}
	pthread_mutex_unlock(&app->state_lock);

	if (stopped > 0)
	{
		char msg[96];
		snprintf(msg, sizeof(msg), "Stopped %u active wiggle(s)", stopped);
		ws_bridge_broadcast_text(app, msg, false);
	}
}

void wiggle_process(app_context_t *app)
{
	if (!app)
	{
		return;
	}

	pthread_mutex_lock(&app->state_lock);
	if (app->state != APP_STATE_PRECHILLING)
	{
		pthread_mutex_unlock(&app->state_lock);
		return;
	}

	uint64_t now = monotonic_ms();
	for (size_t i = 0; i < APP_MAX_WIGGLES; ++i)
	{
		wiggle_command_t *slot = &app->wiggles[i];
		if (!slot->active)
		{
			continue;
		}

		if (slot->total_duration_ms > 0 && slot->stop_time_ms > 0 && now >= slot->stop_time_ms)
		{
			deactivate_slot(app, slot, true);
			continue;
		}

		if (now < slot->next_toggle_ms)
		{
			continue;
		}

		uint16_t next_value = (slot->current_value == slot->start_value) ? slot->end_value : slot->start_value;
		send_value(app, slot, next_value);
		slot->next_toggle_ms = now + slot->period_ms;
	}
	pthread_mutex_unlock(&app->state_lock);
}

#include "control/command.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "can/can_bus.h"
#include "control/firing_sequence.h"
#include "control/state.h"
#include "control/wiggle.h"
#include "util/logger.h"
#include "ws/ws_bridge.h"

static void send_reply(app_context_t *app, ws_client_t *client, const char *msg)
{
	ws_bridge_send_text(app, client, msg);
}

static void send_replyf(app_context_t *app, ws_client_t *client, const char *fmt, ...)
{
	char msg[512];
	va_list args;
	va_start(args, fmt);
	(void)vsnprintf(msg, sizeof(msg), fmt, args);
	va_end(args);
	send_reply(app, client, msg);
}

static const char *strip_can_prefix(const char *text)
{
	if (strncmp(text, "CAN1|", 5) == 0)
	{
		return text + 5;
	}
	return text;
}

static bool handle_read_toggle(app_context_t *app, ws_client_t *client, const char *text)
{
	app_client_ctx_t *ctx = (app_client_ctx_t *)ws_client_get_user_data(client);
	if (!ctx)
	{
		return false;
	}

	if (strcmp(text, "READ-DISABLE") == 0)
	{
		ctx->write_only = true;
		send_reply(app, client, "Write-only mode enabled");
		return true;
	}
	if (strcmp(text, "READ-ENABLE") == 0)
	{
		ctx->write_only = false;
		send_reply(app, client, "Write-only mode disabled");
		return true;
	}
	return false;
}

static bool parse_raw_can_fields(const char *text, uint32_t *id, uint16_t *value, uint16_t *duration_ms, bool *has_duration)
{
	if (!text || !id || !value || !duration_ms || !has_duration)
	{
		return false;
	}

	if (sscanf(text, "%" SCNx32 "#%" SCNx16 "#%" SCNu16, id, value, duration_ms) == 3)
	{
		*has_duration = true;
		return true;
	}

	if (sscanf(text, "%" SCNx32 "#%" SCNx16, id, value) == 2)
	{
		*has_duration = false;
		*duration_ms = 0;
		return true;
	}

	return false;
}

static bool handle_state_command(app_context_t *app, ws_client_t *client, const char *text)
{
	if (strncmp(text, "STATE#", 6) != 0)
	{
		return false;
	}

	const char *requested = text + 6;
	if (strcmp(requested, "PRECHILLING") == 0)
	{
		bool changed = app_state_set(app, APP_STATE_PRECHILLING);
		send_reply(app, client, changed ? "State set to PRECHILLING" : "State set to PRECHILLING (unchanged)");
		return true;
	}
	if (strcmp(requested, "HOTFIRE") == 0)
	{
		bool changed = app_state_set(app, APP_STATE_HOTFIRE);
		send_reply(app, client, changed ? "State set to HOTFIRE" : "State set to HOTFIRE (unchanged)");
		return true;
	}

	send_reply(app, client, "Unknown state requested");
	return true;
}

static bool handle_set_valve(app_context_t *app, ws_client_t *client, const char *text)
{
	if (strncmp(text, "SET_VALVE", 9) != 0)
	{
		return false;
	}

	char valve_name[32];
	uint32_t value = 0;
	if (sscanf(text + 9, "#%31[^#]#%" SCNx32, valve_name, &value) != 2)
	{
		send_reply(app, client, "Error setting valve id");
		return true;
	}

	if (strcmp(valve_name, "LOX_MAIN") == 0)
	{
		if (value > 0x7FFu)
		{
			send_reply(app, client, "Invalid valve id or value");
			return true;
		}
		app->valve_profile.lox_main_valve_id = value;
	}
	else if (strcmp(valve_name, "LOX_VENT") == 0)
	{
		if (value > 0x7FFu)
		{
			send_reply(app, client, "Invalid valve id or value");
			return true;
		}
		app->valve_profile.lox_vent_valve_id = value;
	}
	else if (strcmp(valve_name, "LOX_MAIN_ANGLE_OPEN") == 0)
	{
		if (value > 0x0B4u)
		{
			send_reply(app, client, "Invalid valve id or value");
			return true;
		}
		app->valve_profile.lox_main_angle_open = (uint16_t)value;
	}
	else if (strcmp(valve_name, "LOX_VENT_ANGLE_CLOSE") == 0)
	{
		if (value > 0x0B4u)
		{
			send_reply(app, client, "Invalid valve id or value");
			return true;
		}
		app->valve_profile.lox_vent_angle_close = (uint16_t)value;
	}
	else
	{
		send_reply(app, client, "Invalid valve id or value");
		return true;
	}

	send_reply(app, client, "Valve set successfully");
	return true;
}

static bool handle_drown(app_context_t *app, ws_client_t *client, const char *text)
{
	if (strcmp(text, "DROWN#8765") != 0)
	{
		return false;
	}

	if (firing_sequence_start_drown(app, client) != 0)
	{
		send_reply(app, client, "Error drowning injector");
	}
	return true;
}

static bool handle_abort(app_context_t *app, ws_client_t *client, const char *text)
{
	if (strcmp(text, "ABORT#8765") != 0)
	{
		return false;
	}

	firing_sequence_request_abort(app);
	wiggle_stop_all(app, true);
	send_reply(app, client, "Abort requested");
	return true;
}

static bool handle_fire(app_context_t *app, ws_client_t *client, const char *text)
{
	if (strncmp(text, "FIRE#8765", 9) != 0)
	{
		return false;
	}

	firing_sequence_config_t config;
	if (firing_sequence_parse(text, &config) != 0)
	{
		send_reply(app, client, "Error parsing command");
		return true;
	}

	char started[256];
	int len = snprintf(started, sizeof(started), "Firing sequence started with %u blocks", config.num_blocks);
	for (uint8_t i = 0; i < config.num_blocks && len > 0 && (size_t)len < sizeof(started); ++i)
	{
		len += snprintf(started + len,
										sizeof(started) - (size_t)len,
										"%s%03" PRIx32 "#%04" PRIx16 "#%u",
										i == 0 ? ": " : ", ",
										config.ids[i],
										config.values[i][0],
										config.values[i][1]);
	}

	if (firing_sequence_start(app,
														client,
														&config,
														started,
														"Firing sequence completed",
														"Firing sequence failed to start") != 0)
	{
		send_reply(app, client, "A firing sequence is already running");
	}
	return true;
}

static bool handle_wiggle_stop(app_context_t *app, ws_client_t *client, char *command)
{
	if (strncmp(command, "WIGGLE_STOP", 11) != 0)
	{
		return false;
	}

	char *saveptr = NULL;
	(void)strtok_r(command, "|", &saveptr);
	size_t stopped = 0;
	bool parse_error = false;

	char *token = NULL;
	while ((token = strtok_r(NULL, "|", &saveptr)) != NULL)
	{
		uint32_t id = 0;
		if (sscanf(token, "%" SCNx32, &id) != 1)
		{
			parse_error = true;
			break;
		}
		if (wiggle_stop(app, id))
		{
			++stopped;
		}
	}

	if (parse_error)
	{
		send_reply(app, client, "WIGGLE_STOP failed: invalid format");
	}
	else if (stopped == 0)
	{
		send_reply(app, client, "WIGGLE_STOP: no active wiggles for requested IDs");
	}
	else
	{
		char msg[96];
		snprintf(msg, sizeof(msg), "Stopped wiggle for %zu sensor(s)", stopped);
		send_reply(app, client, msg);
	}
	return true;
}

static bool handle_wiggle(app_context_t *app, ws_client_t *client, char *command)
{
	if (strncmp(command, "WIGGLE", 6) != 0 || strncmp(command, "WIGGLE_STOP", 11) == 0)
	{
		return false;
	}

	if (app_state_get(app) != APP_STATE_PRECHILLING)
	{
		send_reply(app, client, "WIGGLE rejected: system not in PRECHILLING");
		return true;
	}

	char *saveptr = NULL;
	(void)strtok_r(command, "|", &saveptr);
	size_t started = 0;
	bool parse_error = false;

	char *token = NULL;
	while ((token = strtok_r(NULL, "|", &saveptr)) != NULL)
	{
		uint32_t id = 0;
		uint16_t start_value = 0;
		uint16_t end_value = 0;
		uint16_t exit_value = 0;
		uint32_t period_ms = 0;
		uint32_t total_ms = 0;

		int parsed = sscanf(token,
												"%" SCNx32 "#%" SCNx16 "#%" SCNx16 "#%" SCNx16 "#%" SCNu32 "#%" SCNu32,
												&id,
												&start_value,
												&end_value,
												&exit_value,
												&period_ms,
												&total_ms);
		if (parsed == 5)
		{
			total_ms = 0;
		}
		else if (parsed != 6)
		{
			parse_error = true;
			break;
		}

		if (wiggle_start(app, id, start_value, end_value, exit_value, period_ms, total_ms) != 0)
		{
			parse_error = true;
			break;
		}
		++started;
	}

	if (parse_error || started == 0)
	{
		send_reply(app, client, "WIGGLE failed: invalid format or no available slots");
	}
	else
	{
		char msg[96];
		snprintf(msg, sizeof(msg), "WIGGLE started for %zu sensor(s)", started);
		send_reply(app, client, msg);
	}
	return true;
}

static bool handle_raw_can(app_context_t *app, ws_client_t *client, const char *text)
{
	uint32_t id = 0;
	uint16_t value = 0;
	uint16_t duration_ms = 0;
	bool has_duration = false;

	if (!parse_raw_can_fields(text, &id, &value, &duration_ms, &has_duration))
	{
		return false;
	}

	if (can_bus_send_command(app->can_fd, id, value, duration_ms) == 0)
	{
		if (has_duration)
		{
			logger_printf(&app->logger, "CAN_TX", "%03" PRIx32 "#%04" PRIx16 "#%u", id, value, duration_ms);
		}
		else
		{
			logger_printf(&app->logger, "CAN_TX", "%03" PRIx32 "#%04" PRIx16, id, value);
		}
		send_reply(app, client, "Message sent");
	}
	else
	{
		char error_msg[128];
		snprintf(error_msg, sizeof(error_msg), "Error sending message: %s", strerror(errno));
		send_reply(app, client, error_msg);
	}

	return true;
}

static bool handle_testing_state(app_context_t *app, ws_client_t *client, const char *text)
{
	if (strncmp(text, "STATE#", 6) != 0)
	{
		return false;
	}

	const char *requested = text + 6;
	if (strcmp(requested, "PRECHILLING") == 0)
	{
		send_reply(app, client, "TESTING parsed STATE command: PRECHILLING");
		return true;
	}
	if (strcmp(requested, "HOTFIRE") == 0)
	{
		send_reply(app, client, "TESTING parsed STATE command: HOTFIRE");
		return true;
	}

	send_reply(app, client, "TESTING parse error: unknown STATE value");
	return true;
}

static bool handle_testing_set_valve(app_context_t *app, ws_client_t *client, const char *text)
{
	if (strncmp(text, "SET_VALVE", 9) != 0)
	{
		return false;
	}

	char valve_name[32];
	uint32_t value = 0;
	if (sscanf(text + 9, "#%31[^#]#%" SCNx32, valve_name, &value) != 2)
	{
		send_reply(app, client, "TESTING parse error: SET_VALVE format");
		return true;
	}

	if (strcmp(valve_name, "LOX_MAIN") == 0 || strcmp(valve_name, "LOX_VENT") == 0)
	{
		if (value > 0x7FFu)
		{
			send_reply(app, client, "TESTING parse error: valve CAN id out of range");
			return true;
		}
		send_replyf(app, client, "TESTING parsed SET_VALVE: %s=0x%03" PRIx32, valve_name, value);
		return true;
	}

	if (strcmp(valve_name, "LOX_MAIN_ANGLE_OPEN") == 0 || strcmp(valve_name, "LOX_VENT_ANGLE_CLOSE") == 0)
	{
		if (value > 0x0B4u)
		{
			send_reply(app, client, "TESTING parse error: valve angle out of range");
			return true;
		}
		send_replyf(app, client, "TESTING parsed SET_VALVE: %s=0x%03" PRIx32, valve_name, value);
		return true;
	}

	send_reply(app, client, "TESTING parse error: unknown SET_VALVE field");
	return true;
}

static bool handle_testing_wiggle_stop(app_context_t *app, ws_client_t *client, char *command)
{
	if (strncmp(command, "WIGGLE_STOP", 11) != 0)
	{
		return false;
	}

	char *saveptr = NULL;
	(void)strtok_r(command, "|", &saveptr);
	size_t ids = 0;
	bool parse_error = false;

	char *token = NULL;
	while ((token = strtok_r(NULL, "|", &saveptr)) != NULL)
	{
		uint32_t id = 0;
		if (sscanf(token, "%" SCNx32, &id) != 1)
		{
			parse_error = true;
			break;
		}
		++ids;
	}

	if (parse_error || ids == 0)
	{
		send_reply(app, client, "TESTING parse error: WIGGLE_STOP format");
	}
	else
	{
		send_replyf(app, client, "TESTING parsed WIGGLE_STOP: %zu id(s)", ids);
	}
	return true;
}

static bool handle_testing_wiggle(app_context_t *app, ws_client_t *client, char *command)
{
	if (strncmp(command, "WIGGLE", 6) != 0 || strncmp(command, "WIGGLE_STOP", 11) == 0)
	{
		return false;
	}

	char *saveptr = NULL;
	(void)strtok_r(command, "|", &saveptr);
	size_t blocks = 0;
	bool parse_error = false;

	char *token = NULL;
	while ((token = strtok_r(NULL, "|", &saveptr)) != NULL)
	{
		uint32_t id = 0;
		uint16_t start_value = 0;
		uint16_t end_value = 0;
		uint16_t exit_value = 0;
		uint32_t period_ms = 0;
		uint32_t total_ms = 0;

		int parsed = sscanf(token,
											"%" SCNx32 "#%" SCNx16 "#%" SCNx16 "#%" SCNx16 "#%" SCNu32 "#%" SCNu32,
											&id,
											&start_value,
											&end_value,
											&exit_value,
											&period_ms,
											&total_ms);
		if (parsed == 5)
		{
			total_ms = 0;
		}
		else if (parsed != 6)
		{
			parse_error = true;
			break;
		}

		if (id > 0x7FFu || period_ms == 0)
		{
			parse_error = true;
			break;
		}

		++blocks;
	}

	if (parse_error || blocks == 0)
	{
		send_reply(app, client, "TESTING parse error: WIGGLE format");
	}
	else
	{
		send_replyf(app, client, "TESTING parsed WIGGLE: %zu block(s)", blocks);
	}
	return true;
}

static bool handle_testing_fire(app_context_t *app, ws_client_t *client, const char *text)
{
	if (strncmp(text, "FIRE#8765", 9) != 0)
	{
		return false;
	}

	firing_sequence_config_t config;
	if (firing_sequence_parse(text, &config) != 0)
	{
		send_reply(app, client, "TESTING parse error: FIRE format");
		return true;
	}

	char msg[512];
	int len = snprintf(msg, sizeof(msg), "TESTING parsed FIRE: %u block(s)", config.num_blocks);
	for (uint8_t i = 0; i < config.num_blocks && len > 0 && (size_t)len < sizeof(msg); ++i)
	{
		len += snprintf(msg + len,
								 sizeof(msg) - (size_t)len,
								 "%s%03" PRIx32 "#%04" PRIx16 "#%u",
								 i == 0 ? " (" : ", ",
								 config.ids[i],
								 config.values[i][0],
								 config.values[i][1]);
	}
	if (len > 0 && (size_t)len < sizeof(msg))
	{
		len += snprintf(msg + len, sizeof(msg) - (size_t)len, ")");
		(void)len;
	}

	send_reply(app, client, msg);
	return true;
}

static bool handle_testing_drown(app_context_t *app, ws_client_t *client, const char *text)
{
	if (strcmp(text, "DROWN#8765") != 0)
	{
		return false;
	}

	send_reply(app, client, "TESTING parsed DROWN command");
	return true;
}

static bool handle_testing_abort(app_context_t *app, ws_client_t *client, const char *text)
{
	if (strcmp(text, "ABORT#8765") != 0)
	{
		return false;
	}

	send_reply(app, client, "TESTING parsed ABORT command");
	return true;
}

static bool handle_testing_raw_can(app_context_t *app, ws_client_t *client, const char *text)
{
	uint32_t id = 0;
	uint16_t value = 0;
	uint16_t duration_ms = 0;
	bool has_duration = false;

	if (!parse_raw_can_fields(text, &id, &value, &duration_ms, &has_duration))
	{
		return false;
	}

	if (id > 0x7FFu)
	{
		send_reply(app, client, "TESTING parse error: CAN id out of range");
		return true;
	}

	if (has_duration)
	{
		send_replyf(app,
						client,
						"TESTING parsed RAW CAN: id=0x%03" PRIx32 " value=0x%04" PRIx16 " duration=%u",
						id,
						value,
						duration_ms);
	}
	else
	{
		send_replyf(app,
						client,
						"TESTING parsed RAW CAN: id=0x%03" PRIx32 " value=0x%04" PRIx16,
						id,
						value);
	}

	return true;
}

static void handle_testing_command(app_context_t *app, ws_client_t *client, const char *normalized)
{
	if (handle_testing_abort(app, client, normalized))
	{
		return;
	}
	if (handle_testing_state(app, client, normalized))
	{
		return;
	}
	if (handle_testing_set_valve(app, client, normalized))
	{
		return;
	}

	char *mutable = strdup(normalized);
	if (!mutable)
	{
		send_reply(app, client, "Server error: out of memory");
		return;
	}

	if (handle_testing_wiggle_stop(app, client, mutable))
	{
		free(mutable);
		return;
	}
	strcpy(mutable, normalized);
	if (handle_testing_wiggle(app, client, mutable))
	{
		free(mutable);
		return;
	}
	free(mutable);

	if (handle_testing_drown(app, client, normalized))
	{
		return;
	}
	if (handle_testing_fire(app, client, normalized))
	{
		return;
	}
	if (handle_testing_raw_can(app, client, normalized))
	{
		return;
	}

	send_reply(app, client, "TESTING invalid command");
}

void command_handle_text(app_context_t *app, ws_client_t *client, const char *text)
{
	if (!app || !client || !text)
	{
		return;
	}

	if (handle_read_toggle(app, client, text))
	{
		return;
	}

	const char *normalized = strip_can_prefix(text);
	if (app->config.testing_mode)
	{
		handle_testing_command(app, client, normalized);
		return;
	}

	if (app->config.read_only)
	{
		send_reply(app, client, "The client is not allowed to execute commands");
		return;
	}

	if (handle_abort(app, client, normalized))
	{
		return;
	}
	if (handle_state_command(app, client, normalized))
	{
		return;
	}
	if (handle_set_valve(app, client, normalized))
	{
		return;
	}

	char *mutable = strdup(normalized);
	if (!mutable)
	{
		send_reply(app, client, "Server error: out of memory");
		return;
	}

	if (handle_wiggle_stop(app, client, mutable))
	{
		free(mutable);
		return;
	}
	strcpy(mutable, normalized);
	if (handle_wiggle(app, client, mutable))
	{
		free(mutable);
		return;
	}
	free(mutable);

	if (app_state_get(app) == APP_STATE_PRECHILLING)
	{
		send_reply(app, client, "System is in PRE-HOTFIRE state; command ignored.");
		return;
	}

	if (handle_drown(app, client, normalized))
	{
		return;
	}
	if (handle_fire(app, client, normalized))
	{
		return;
	}
	if (handle_raw_can(app, client, normalized))
	{
		return;
	}

	send_reply(app, client, "Invalid command");
}

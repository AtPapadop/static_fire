#define _POSIX_C_SOURCE 200809L

#include "ws/ws_bridge.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "control/command.h"
#include "control/state.h"
#include "util/logger.h"

static char *dup_payload_text(const uint8_t *payload, size_t len)
{
	char *text = malloc(len + 1);
	if (!text)
	{
		return NULL;
	}
	if (len > 0 && payload)
	{
		memcpy(text, payload, len);
	}
	text[len] = '\0';
	return text;
}

static void on_connect(ws_server_t *server, ws_client_t *client, void *user_data)
{
	(void)server;
	app_context_t *app = (app_context_t *)user_data;
	if (!app)
	{
		return;
	}

	app_client_ctx_t *ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
	{
		ws_server_close_client(server, client, "server memory error");
		return;
	}

	ctx->client = client;
	ws_client_set_user_data(client, ctx);

	pthread_mutex_lock(&app->clients_lock);
	ctx->next = app->clients;
	app->clients = ctx;
	pthread_mutex_unlock(&app->clients_lock);

	char ip[64];
	const char *addr = ws_client_ip(client, ip, sizeof(ip));
	printf("Client connected: %s:%u\n", addr ? addr : "unknown", ws_client_port(client));

	ws_bridge_send_text(app, client, "Hello from server");
	ws_bridge_send_current_state(app, client);
}

static void on_disconnect(ws_server_t *server, ws_client_t *client, const char *reason, void *user_data)
{
	(void)server;
	app_context_t *app = (app_context_t *)user_data;
	if (!app)
	{
		return;
	}

	char ip[64];
	const char *addr = ws_client_ip(client, ip, sizeof(ip));
	printf("Client disconnected: %s:%u (%s)\n",
				 addr ? addr : "unknown",
				 ws_client_port(client),
				 reason ? reason : "no reason");

	pthread_mutex_lock(&app->clients_lock);
	app_client_ctx_t *ctx = (app_client_ctx_t *)ws_client_get_user_data(client);
	app_client_ctx_t **pp = &app->clients;
	while (*pp)
	{
		if (*pp == ctx)
		{
			*pp = ctx->next;
			break;
		}
		pp = &(*pp)->next;
	}
	pthread_mutex_unlock(&app->clients_lock);

	ws_client_set_user_data(client, NULL);
	free(ctx);
}

static void on_error(ws_server_t *server, const char *where, int error_code, void *user_data)
{
	(void)server;
	(void)user_data;
	if (error_code)
	{
		fprintf(stderr, "ws error at %s: %s\n", where ? where : "unknown", strerror(error_code));
	}
	else
	{
		fprintf(stderr, "ws error at %s\n", where ? where : "unknown");
	}
}

static void on_handshake_fail(ws_server_t *server, ws_client_t *client, const char *reason, void *user_data)
{
	(void)server;
	(void)user_data;
	char ip[64];
	const char *addr = ws_client_ip(client, ip, sizeof(ip));
	fprintf(stderr, "Handshake failed for %s:%u (%s)\n",
					addr ? addr : "unknown",
					ws_client_port(client),
					reason ? reason : "unknown");
}

static void on_message(ws_server_t *server, ws_client_t *client, const ws_frame_t *frame, void *user_data)
{
	(void)server;
	app_context_t *app = (app_context_t *)user_data;
	if (!app || !frame)
	{
		return;
	}

	if (frame->type != WS_TEXT_FRAME)
	{
		return;
	}

	char *text = dup_payload_text(frame->payload, frame->payload_length);
	if (!text)
	{
		ws_bridge_send_text(app, client, "Server error: out of memory");
		return;
	}

	logger_write(&app->logger, "WS_RX", text, strlen(text));
	command_handle_text(app, client, text);
	free(text);
}

int ws_bridge_start(app_context_t *app)
{
	if (!app)
	{
		return -1;
	}

	app->ws = ws_server_create(app->config.ws_port, app->config.max_clients);
	if (!app->ws)
	{
		return -1;
	}

	ws_server_set_user_data(app->ws, app);
	ws_server_set_connect_handler(app->ws, on_connect);
	ws_server_set_disconnect_handler(app->ws, on_disconnect);
	ws_server_set_message_handler(app->ws, on_message);
	ws_server_set_error_handler(app->ws, on_error);
	ws_server_set_handshake_fail_handler(app->ws, on_handshake_fail);
	ws_server_set_backlog(app->ws, app->config.max_clients);
	ws_server_set_initial_buffer_size(app->ws, 4096);

	return ws_server_start(app->ws);
}

void ws_bridge_stop(app_context_t *app)
{
	if (!app || !app->ws)
	{
		return;
	}

	ws_server_stop(app->ws);
	ws_server_join(app->ws);
	ws_server_destroy(app->ws);
	app->ws = NULL;

	pthread_mutex_lock(&app->clients_lock);
	app_client_ctx_t *node = app->clients;
	while (node)
	{
		app_client_ctx_t *next = node->next;
		free(node);
		node = next;
	}
	app->clients = NULL;
	pthread_mutex_unlock(&app->clients_lock);
}

void ws_bridge_send_text(app_context_t *app, ws_client_t *client, const char *msg)
{
	if (!app || !app->ws || !client || !msg)
	{
		return;
	}
	if (ws_client_is_connected(client) && ws_client_handshake_done(client))
	{
		ws_server_send_text(app->ws, client, msg);
	}
}

void ws_bridge_send_current_state(app_context_t *app, ws_client_t *client)
{
	char msg[64];
	snprintf(msg, sizeof(msg), "State has been set to %s", app_state_label(app_state_get(app)));
	ws_bridge_send_text(app, client, msg);
}

void ws_bridge_broadcast_text(app_context_t *app, const char *msg, bool prefix_timestamp)
{
	if (!app || !msg)
	{
		return;
	}

	char stack_buf[2048];
	char *heap_buf = NULL;
	const char *payload = msg;

	if (prefix_timestamp)
	{
		size_t needed = strlen(msg) + 32;
		char *target = needed <= sizeof(stack_buf) ? stack_buf : malloc(needed);
		if (target)
		{
			snprintf(target, needed, "%llu|%s", (unsigned long long)realtime_ns(), msg);
			payload = target;
			if (target != stack_buf)
			{
				heap_buf = target;
			}
		}
	}

	logger_write(&app->logger, "WS_TX", payload, strlen(payload));

	pthread_mutex_lock(&app->clients_lock);
	ws_server_broadcast_text(app->ws, payload);
	pthread_mutex_unlock(&app->clients_lock);

	free(heap_buf);
}

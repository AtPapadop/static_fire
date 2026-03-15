#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <linux/can.h>

#include "app/config.h"
#include "app/context.h"
#include "can/can_bus.h"
#include "control/firing_sequence.h"
#include "control/wiggle.h"
#include "util/heartbeat.h"
#include "util/logger.h"
#include "ws/ws_bridge.h"

static app_context_t *g_app = NULL;

static void on_signal(int sig)
{
	(void)sig;
	if (g_app)
	{
		g_app->stop = 1;
	}
}

static int register_fd(int epoll_fd, int fd)
{
	struct epoll_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.fd = fd;
	return epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);
}

static int init_runtime(app_context_t *app)
{
	if (app->config.auto_config_can)
	{
		if (can_bus_configure_interface(app->config.can_ifname, app->config.can_bitrate) != 0)
		{
			perror("can_bus_configure_interface");
			return -1;
		}
	}

	app->can_fd = can_bus_open(app->config.can_ifname);
	if (app->can_fd < 0)
	{
		perror("can_bus_open");
		return -1;
	}

	app->epoll_fd = epoll_create1(0);
	if (app->epoll_fd < 0)
	{
		perror("epoll_create1");
		return -1;
	}

	if (register_fd(app->epoll_fd, app->can_fd) != 0)
	{
		perror("epoll_ctl can_fd");
		return -1;
	}

	app->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if (app->timer_fd < 0)
	{
		perror("timerfd_create");
		return -1;
	}

	struct itimerspec ts;
	memset(&ts, 0, sizeof(ts));
	ts.it_value.tv_sec = app->config.tick_interval_ms / 1000;
	ts.it_value.tv_nsec = (app->config.tick_interval_ms % 1000) * 1000000L;
	ts.it_interval = ts.it_value;

	if (timerfd_settime(app->timer_fd, 0, &ts, NULL) != 0)
	{
		perror("timerfd_settime");
		return -1;
	}

	if (register_fd(app->epoll_fd, app->timer_fd) != 0)
	{
		perror("epoll_ctl timer_fd");
		return -1;
	}

	return 0;
}

static void shutdown_runtime(app_context_t *app)
{
	if (!app)
	{
		return;
	}

	firing_sequence_request_abort(app);
	wiggle_stop_all(app, true);
	ws_bridge_stop(app);

	if (app->timer_fd >= 0)
	{
		close(app->timer_fd);
		app->timer_fd = -1;
	}
	if (app->epoll_fd >= 0)
	{
		close(app->epoll_fd);
		app->epoll_fd = -1;
	}
	can_bus_close(&app->can_fd);
	logger_close(&app->logger);
}

static void flush_can_updates(app_context_t *app)
{
	char buffer[8192];
	size_t len = can_bus_format_dirty(app, buffer, sizeof(buffer));
	if (len > 0)
	{
		ws_bridge_broadcast_text(app, buffer, true);
	}
}

int main(int argc, char **argv)
{
	app_config_t cfg;
	app_config_init_defaults(&cfg);
	if (app_config_parse(&cfg, argc, argv) != 0)
	{
		fprintf(stderr, "Invalid arguments\n");
		return EXIT_FAILURE;
	}

	app_context_t app;
	app_context_init(&app, &cfg);
	g_app = &app;

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	app_config_print(&cfg);

	if (logger_open(&app.logger, cfg.logging_enabled, cfg.log_path) != 0)
	{
		perror("logger_open");
		app_context_cleanup(&app);
		return EXIT_FAILURE;
	}

	if (init_runtime(&app) != 0)
	{
		shutdown_runtime(&app);
		app_context_cleanup(&app);
		return EXIT_FAILURE;
	}

	if (ws_bridge_start(&app) != 0)
	{
		fprintf(stderr, "Failed to start WebSocket server\n");
		shutdown_runtime(&app);
		app_context_cleanup(&app);
		return EXIT_FAILURE;
	}

	printf("Listening on ws://0.0.0.0:%u using CAN interface %s\n", cfg.ws_port, cfg.can_ifname);
	uint64_t next_heartbeat_ms = heartbeat_now_ms() + HEARTBEAT_PERIOD_MS;

	struct epoll_event events[8];
	while (!app.stop)
	{
		int n = epoll_wait(app.epoll_fd, events, 8, 500);
		if (n < 0)
		{
			if (errno == EINTR)
			{
				continue;
			}
			perror("epoll_wait");
			break;
		}

		for (int i = 0; i < n; ++i)
		{
			if (events[i].data.fd == app.can_fd)
			{
				for (;;)
				{
					struct can_frame frame;
					int rc = can_bus_read_frame(app.can_fd, &frame);
					if (rc == 1)
					{
						can_bus_store_rx(&app, &frame);
					}
					else if (rc == 0)
					{
						break;
					}
					else
					{
						if (errno != EAGAIN && errno != EWOULDBLOCK)
						{
							perror("can read");
						}
						break;
					}
				}
			}
			else if (events[i].data.fd == app.timer_fd)
			{
				uint64_t expirations = 0;
				(void)read(app.timer_fd, &expirations, sizeof(expirations));
				wiggle_process(&app);

				uint64_t now_ms = heartbeat_now_ms();
				if (now_ms >= next_heartbeat_ms)
				{
					if (heartbeat_send(app.can_fd) != 0)
					{
						perror("heartbeat_send");
					}

					do
					{
						next_heartbeat_ms += HEARTBEAT_PERIOD_MS;
					} while (next_heartbeat_ms <= now_ms);
				}

				flush_can_updates(&app);
			}
		}
	}

	shutdown_runtime(&app);
	app_context_cleanup(&app);
	return EXIT_SUCCESS;
}

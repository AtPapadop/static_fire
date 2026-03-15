#define _POSIX_C_SOURCE 200809L

#include "can/can_bus.h"

#include <errno.h>
#include <net/if.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <linux/can/raw.h>
#include <linux/can/error.h>
#include <linux/if.h>

#include "app/context.h"
#include "util/logger.h"

static int run_command(char *const argv[])
{
	pid_t pid = fork();
	if (pid < 0)
	{
		return -1;
	}

	if (pid == 0)
	{
		execvp(argv[0], argv);
		_exit((errno == ENOENT) ? 127 : 126);
	}

	int status = 0;
	while (waitpid(pid, &status, 0) < 0)
	{
		if (errno == EINTR)
		{
			continue;
		}
		return -1;
	}

	if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
	{
		return 0;
	}

	errno = EIO;
	return -1;
}

int can_bus_configure_interface(const char *ifname, uint32_t bitrate)
{
	if (!ifname || !*ifname || bitrate == 0u)
	{
		errno = EINVAL;
		return -1;
	}

	char bitrate_str[16];
	(void)snprintf(bitrate_str, sizeof(bitrate_str), "%u", bitrate);

	char *down_cmd[] = {"ip", "link", "set", "dev", (char *)ifname, "down", NULL};
	char *type_cmd[] = {"ip", "link", "set", "dev", (char *)ifname, "type", "can", "bitrate", bitrate_str, "restart-ms", "100", NULL};
	char *up_cmd[] = {"ip", "link", "set", "dev", (char *)ifname, "up", NULL};

	if (run_command(down_cmd) != 0)
	{
		return -1;
	}
	if (run_command(type_cmd) != 0)
	{
		return -1;
	}
	if (run_command(up_cmd) != 0)
	{
		return -1;
	}

	return 0;
}

int can_bus_open(const char *ifname)
{
	if (!ifname || !*ifname)
	{
		errno = EINVAL;
		return -1;
	}

	int fd = socket(PF_CAN, SOCK_RAW | SOCK_NONBLOCK, CAN_RAW);
	if (fd < 0)
	{
		return -1;
	}

	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
	if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0)
	{
		close(fd);
		return -1;
	}

	int recv_own_msgs = 0;
	(void)setsockopt(fd, SOL_CAN_RAW, CAN_RAW_RECV_OWN_MSGS, &recv_own_msgs, sizeof(recv_own_msgs));

	struct sockaddr_can addr;
	memset(&addr, 0, sizeof(addr));
	addr.can_family = AF_CAN;
	addr.can_ifindex = ifr.ifr_ifindex;

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
	{
		close(fd);
		return -1;
	}

	return fd;
}

void can_bus_close(int *fd)
{
	if (!fd || *fd < 0)
	{
		return;
	}
	close(*fd);
	*fd = -1;
}

int can_bus_send_command(int fd, uint32_t can_id, uint16_t value, uint16_t duration_ms)
{
	if (fd < 0 || can_id > 0x7FFu)
	{
		errno = EINVAL;
		return -1;
	}

	struct can_frame frame;
	memset(&frame, 0, sizeof(frame));
	frame.can_id = can_id;
	frame.can_dlc = 8;
	frame.data[0] = (uint8_t)(value & 0xFFu);
	frame.data[1] = (uint8_t)((value >> 8) & 0xFFu);
	frame.data[2] = (uint8_t)(duration_ms & 0xFFu);
	frame.data[3] = (uint8_t)((duration_ms >> 8) & 0xFFu);

	ssize_t written = write(fd, &frame, sizeof(frame));
	return (written == (ssize_t)sizeof(frame)) ? 0 : -1;
}

int can_bus_read_frame(int fd, struct can_frame *frame)
{
	if (fd < 0 || !frame)
	{
		errno = EINVAL;
		return -1;
	}

	ssize_t n = read(fd, frame, sizeof(*frame));
	if (n == (ssize_t)sizeof(*frame))
	{
		return 1;
	}
	if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
	{
		return 0;
	}
	return -1;
}

void can_bus_store_rx(struct app_context *app, const struct can_frame *frame)
{
	if (!app || !frame)
	{
		return;
	}

	if (frame->can_id & (CAN_ERR_FLAG | CAN_RTR_FLAG | CAN_EFF_FLAG))
	{
		return;
	}

	uint32_t id = frame->can_id & 0x7FFu;
	uint16_t value = 0;
	if (frame->can_dlc >= 2)
	{
		value = (uint16_t)frame->data[0] | ((uint16_t)frame->data[1] << 8);
	}

	pthread_mutex_lock(&app->can_lock);
	app->can_values[id] = (int32_t)value;
	app->can_dirty[id] = true;
	pthread_mutex_unlock(&app->can_lock);

logger_printf(&app->logger, "CAN_RX", "%03" PRIx32 "#%04" PRIx16, id, value);
}

size_t can_bus_format_dirty(struct app_context *app, char *buffer, size_t capacity)
{
	if (!app || !buffer || capacity == 0)
	{
		return 0;
	}

	size_t length = 0;
	buffer[0] = '\0';

	pthread_mutex_lock(&app->can_lock);
	for (uint32_t id = 0; id < APP_CAN_ID_COUNT; ++id)
	{
		if (!app->can_dirty[id] || app->can_values[id] < 0)
		{
			continue;
		}

		int written = snprintf(buffer + length,
													 capacity - length,
													 "%03" PRIx32 "#%04" PRIx16 "|",
													 id,
													 (uint16_t)app->can_values[id]);
		if (written < 0 || (size_t)written >= capacity - length)
		{
			break;
		}

		length += (size_t)written;
		app->can_dirty[id] = false;
	}
	pthread_mutex_unlock(&app->can_lock);

	if (length > 0)
	{
		buffer[length - 1] = '\0';
		--length;
	}

	return length;
}

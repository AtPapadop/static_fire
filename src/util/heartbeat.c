#define _POSIX_C_SOURCE 200809L

#include "util/heartbeat.h"

#include <errno.h>
#include <time.h>

#include "can/can_bus.h"

uint64_t heartbeat_now_ms(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
	{
		return 0;
	}

	return ((uint64_t)ts.tv_sec * 1000u) + ((uint64_t)ts.tv_nsec / 1000000u);
}

int heartbeat_send(int can_fd)
{
	if (can_fd < 0)
	{
		errno = EINVAL;
		return -1;
	}

	return can_bus_send_command(can_fd, HEARTBEAT_CAN_ID, HEARTBEAT_VALUE, 0u);
}

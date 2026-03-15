#ifndef UTIL_HEARTBEAT_H
#define UTIL_HEARTBEAT_H

#include <stdint.h>

#define HEARTBEAT_CAN_ID 0x001u
#define HEARTBEAT_VALUE 0x0001u
#define HEARTBEAT_PERIOD_MS 1000u

uint64_t heartbeat_now_ms(void);
int heartbeat_send(int can_fd);

#endif

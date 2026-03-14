#ifndef CAN_BUS_H
#define CAN_BUS_H

#include <stdint.h>
#include <stddef.h>
#include <linux/can.h>

struct app_context;

int can_bus_open(const char *ifname);
void can_bus_close(int *fd);
int can_bus_send_command(int fd, uint32_t can_id, uint16_t value, uint16_t duration_ms);
int can_bus_read_frame(int fd, struct can_frame *frame);
void can_bus_store_rx(struct app_context *app, const struct can_frame *frame);
size_t can_bus_format_dirty(struct app_context *app, char *buffer, size_t capacity);

#endif

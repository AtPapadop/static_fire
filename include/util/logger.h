#ifndef UTIL_LOGGER_H
#define UTIL_LOGGER_H

#include <stdbool.h>
#include <stdio.h>
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <sys/types.h>

typedef struct {
    bool enabled;
    FILE *fp;
    pthread_mutex_t lock;
    char log_dir[PATH_MAX];
    char current_path[PATH_MAX];
    uint64_t next_maintenance_ms;
    bool cyclic_mode;
    off_t cyclic_write_offset;
    off_t cyclic_capacity_bytes;
} logger_t;

int logger_open(logger_t *logger, bool enabled, const char *log_dir);
void logger_close(logger_t *logger);
void logger_write(logger_t *logger, const char *kind, const void *payload, size_t len);
void logger_printf(logger_t *logger, const char *kind, const char *fmt, ...);
uint64_t realtime_ns(void);
uint64_t monotonic_ms(void);

#endif

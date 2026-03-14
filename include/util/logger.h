#ifndef UTIL_LOGGER_H
#define UTIL_LOGGER_H

#include <stdbool.h>
#include <stdio.h>
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>

typedef struct {
    bool enabled;
    FILE *fp;
    pthread_mutex_t lock;
} logger_t;

int logger_open(logger_t *logger, bool enabled, const char *path);
void logger_close(logger_t *logger);
void logger_write(logger_t *logger, const char *kind, const void *payload, size_t len);
void logger_printf(logger_t *logger, const char *kind, const char *fmt, ...);
uint64_t realtime_ns(void);
uint64_t monotonic_ms(void);

#endif

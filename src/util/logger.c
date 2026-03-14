#define _POSIX_C_SOURCE 200809L

#include "util/logger.h"

#include <errno.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

uint64_t realtime_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

uint64_t monotonic_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

int logger_open(logger_t *logger, bool enabled, const char *path)
{
	if (!logger)
	{
		return -1;
	}

	memset(logger, 0, sizeof(*logger));
	logger->enabled = enabled;
	pthread_mutex_init(&logger->lock, NULL);

	if (!enabled)
	{
		return 0;
	}

	logger->fp = fopen(path, "a");
	if (!logger->fp)
	{
		pthread_mutex_destroy(&logger->lock);
		return -1;
	}

	setvbuf(logger->fp, NULL, _IOLBF, 0);
	return 0;
}

void logger_close(logger_t *logger)
{
	if (!logger)
	{
		return;
	}

	pthread_mutex_lock(&logger->lock);
	if (logger->fp)
	{
		fflush(logger->fp);
		fsync(fileno(logger->fp));
		fclose(logger->fp);
		logger->fp = NULL;
	}
	pthread_mutex_unlock(&logger->lock);
	pthread_mutex_destroy(&logger->lock);
}

void logger_write(logger_t *logger, const char *kind, const void *payload, size_t len)
{
	if (!logger || !logger->enabled || !logger->fp || !kind)
	{
		return;
	}

	pthread_mutex_lock(&logger->lock);
	fprintf(logger->fp, "%llu,%s,", (unsigned long long)realtime_ns(), kind);
	if (payload && len > 0)
	{
		fwrite(payload, 1, len, logger->fp);
	}
	fputc('\n', logger->fp);
	pthread_mutex_unlock(&logger->lock);
}

void logger_printf(logger_t *logger, const char *kind, const char *fmt, ...)
{
	if (!logger || !logger->enabled || !logger->fp || !kind || !fmt)
	{
		return;
	}

	pthread_mutex_lock(&logger->lock);
	fprintf(logger->fp, "%llu,%s,", (unsigned long long)realtime_ns(), kind);

	va_list ap;
	va_start(ap, fmt);
	vfprintf(logger->fp, fmt, ap);
	va_end(ap);

	fputc('\n', logger->fp);
	pthread_mutex_unlock(&logger->lock);
}

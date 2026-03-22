#define _POSIX_C_SOURCE 200809L

#include "util/logger.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int ensure_directory_exists(const char *path)
{
	if (!path || *path == '\0')
	{
		errno = EINVAL;
		return -1;
	}

	char tmp[PATH_MAX];
	size_t len = strlen(path);
	if (len == 0 || len >= sizeof(tmp))
	{
		errno = ENAMETOOLONG;
		return -1;
	}

	memcpy(tmp, path, len + 1);

	for (char *p = tmp + 1; *p != '\0'; ++p)
	{
		if (*p != '/')
		{
			continue;
		}

		*p = '\0';
		if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
		{
			return -1;
		}
		*p = '/';
	}

	if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
	{
		return -1;
	}

	return 0;
}

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

int logger_open(logger_t *logger, bool enabled, const char *log_dir)
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

	if (!log_dir || *log_dir == '\0')
	{
		errno = EINVAL;
		pthread_mutex_destroy(&logger->lock);
		return -1;
	}

	if (ensure_directory_exists(log_dir) != 0)
	{
		pthread_mutex_destroy(&logger->lock);
		return -1;
	}

	time_t now = time(NULL);
	if (now == (time_t)-1)
	{
		pthread_mutex_destroy(&logger->lock);
		return -1;
	}

	struct tm tm_now;
	if (!localtime_r(&now, &tm_now))
	{
		pthread_mutex_destroy(&logger->lock);
		return -1;
	}

	char filename[64];
	if (strftime(filename, sizeof(filename), "simple_ws_%Y%m%d_%H%M%S.csv", &tm_now) == 0)
	{
		errno = EINVAL;
		pthread_mutex_destroy(&logger->lock);
		return -1;
	}

	char full_path[PATH_MAX];
	int n = snprintf(full_path, sizeof(full_path), "%s%s%s",
							 log_dir,
							 (log_dir[strlen(log_dir) - 1] == '/') ? "" : "/",
							 filename);
	if (n < 0 || (size_t)n >= sizeof(full_path))
	{
		errno = ENAMETOOLONG;
		pthread_mutex_destroy(&logger->lock);
		return -1;
	}

	logger->fp = fopen(full_path, "a");
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

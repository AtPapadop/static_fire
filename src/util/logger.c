#define _POSIX_C_SOURCE 200809L

#include "util/logger.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/statvfs.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LOGGER_MIN_FREE_PERCENT 20U
#define LOGGER_MAINTENANCE_INTERVAL_MS 5000ULL
#define LOGGER_MIN_CYCLIC_CAPACITY_BYTES 4096

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

static int get_free_percent(const char *path, unsigned int *out_percent)
{
	if (!path || !out_percent)
	{
		errno = EINVAL;
		return -1;
	}

	struct statvfs fs;
	if (statvfs(path, &fs) != 0)
	{
		return -1;
	}

	if (fs.f_blocks == 0)
	{
		*out_percent = 100U;
		return 0;
	}

	*out_percent = (unsigned int)(((unsigned long long)fs.f_bavail * 100ULL) / (unsigned long long)fs.f_blocks);
	return 0;
}

static bool is_logger_file_name(const char *name)
{
	if (!name)
	{
		return false;
	}

	return strncmp(name, "simple_ws_", 10) == 0;
}

static int timespec_cmp(const struct timespec *a, const struct timespec *b)
{
	if (a->tv_sec < b->tv_sec)
	{
		return -1;
	}
	if (a->tv_sec > b->tv_sec)
	{
		return 1;
	}
	if (a->tv_nsec < b->tv_nsec)
	{
		return -1;
	}
	if (a->tv_nsec > b->tv_nsec)
	{
		return 1;
	}
	return 0;
}

static int find_oldest_deletable_log(const logger_t *logger, char *out_path, size_t out_size, bool *found)
{
	if (!logger || !out_path || out_size == 0 || !found)
	{
		errno = EINVAL;
		return -1;
	}

	*found = false;
	DIR *dir = opendir(logger->log_dir);
	if (!dir)
	{
		return -1;
	}

	struct timespec oldest_mtime;
	char oldest_path[PATH_MAX] = {0};

	for (;;)
	{
		errno = 0;
		struct dirent *ent = readdir(dir);
		if (!ent)
		{
			if (errno != 0)
			{
				closedir(dir);
				return -1;
			}
			break;
		}

		if (!is_logger_file_name(ent->d_name))
		{
			continue;
		}

		char full_path[PATH_MAX];
		int n = snprintf(full_path, sizeof(full_path), "%s%s%s",
						 logger->log_dir,
						 (logger->log_dir[strlen(logger->log_dir) - 1] == '/') ? "" : "/",
						 ent->d_name);
		if (n < 0 || (size_t)n >= sizeof(full_path))
		{
			continue;
		}

		if (strcmp(full_path, logger->current_path) == 0)
		{
			continue;
		}

		struct stat st;
		if (stat(full_path, &st) != 0)
		{
			continue;
		}
		if (!S_ISREG(st.st_mode))
		{
			continue;
		}

		if (!*found || timespec_cmp(&st.st_mtim, &oldest_mtime) < 0)
		{
			oldest_mtime = st.st_mtim;
			snprintf(oldest_path, sizeof(oldest_path), "%s", full_path);
			*found = true;
		}
	}

	closedir(dir);
	if (!*found)
	{
		return 0;
	}

	snprintf(out_path, out_size, "%s", oldest_path);
	return 0;
}

static int prepare_cyclic_mode_locked(logger_t *logger)
{
	if (!logger || !logger->fp)
	{
		errno = EINVAL;
		return -1;
	}

	if (fflush(logger->fp) != 0)
	{
		return -1;
	}
	struct stat st;
	if (fstat(fileno(logger->fp), &st) != 0)
	{
		return -1;
	}

	off_t cap = st.st_size;
	if (cap < (off_t)LOGGER_MIN_CYCLIC_CAPACITY_BYTES)
	{
		cap = (off_t)LOGGER_MIN_CYCLIC_CAPACITY_BYTES;
	}

	logger->cyclic_capacity_bytes = cap;
	if (logger->cyclic_write_offset < 0 || logger->cyclic_write_offset >= logger->cyclic_capacity_bytes)
	{
		logger->cyclic_write_offset = 0;
	}
	return 0;
}

static int write_cyclic_bytes_locked(logger_t *logger, const char *data, size_t len)
{
	if (!logger || !logger->fp || !data)
	{
		errno = EINVAL;
		return -1;
	}

	if (len == 0)
	{
		return 0;
	}

	if (logger->cyclic_capacity_bytes <= 0)
	{
		if (prepare_cyclic_mode_locked(logger) != 0)
		{
			return -1;
		}
	}

	if ((off_t)len > logger->cyclic_capacity_bytes)
	{
		logger->cyclic_capacity_bytes = (off_t)len;
	}

	if (logger->cyclic_write_offset + (off_t)len > logger->cyclic_capacity_bytes)
	{
		logger->cyclic_write_offset = 0;
	}

	if (fseeko(logger->fp, logger->cyclic_write_offset, SEEK_SET) != 0)
	{
		return -1;
	}

	if (fwrite(data, 1, len, logger->fp) != len)
	{
		clearerr(logger->fp);
		return -1;
	}

	logger->cyclic_write_offset += (off_t)len;
	if (logger->cyclic_write_offset >= logger->cyclic_capacity_bytes)
	{
		logger->cyclic_write_offset = 0;
	}

	if (fflush(logger->fp) != 0)
	{
		return -1;
	}

	return 0;
}

static int write_normal_line_locked(logger_t *logger, const char *kind, const void *payload, size_t len)
{
	if (fseeko(logger->fp, 0, SEEK_END) != 0)
	{
		return -1;
	}

	fprintf(logger->fp, "%llu,%s,", (unsigned long long)realtime_ns(), kind);
	if (payload && len > 0)
	{
		fwrite(payload, 1, len, logger->fp);
	}
	fputc('\n', logger->fp);
	return 0;
}

static void maybe_run_maintenance_locked(logger_t *logger)
{
	if (!logger || !logger->enabled || !logger->fp)
	{
		return;
	}

	uint64_t now_ms = monotonic_ms();
	if (now_ms < logger->next_maintenance_ms)
	{
		return;
	}
	logger->next_maintenance_ms = now_ms + LOGGER_MAINTENANCE_INTERVAL_MS;

	unsigned int free_pct = 100U;
	if (get_free_percent(logger->log_dir, &free_pct) != 0)
	{
		return;
	}

	if (free_pct >= LOGGER_MIN_FREE_PERCENT)
	{
		logger->cyclic_mode = false;
		logger->cyclic_capacity_bytes = 0;
		logger->cyclic_write_offset = 0;
		return;
	}

	for (;;)
	{
		char oldest_path[PATH_MAX] = {0};
		bool found = false;

		if (find_oldest_deletable_log(logger, oldest_path, sizeof(oldest_path), &found) != 0)
		{
			break;
		}

		if (!found)
		{
			logger->cyclic_mode = true;
			(void)prepare_cyclic_mode_locked(logger);
			break;
		}

		if (unlink(oldest_path) != 0)
		{
			break;
		}

		if (get_free_percent(logger->log_dir, &free_pct) != 0)
		{
			break;
		}

		if (free_pct >= LOGGER_MIN_FREE_PERCENT)
		{
			logger->cyclic_mode = false;
			logger->cyclic_capacity_bytes = 0;
			logger->cyclic_write_offset = 0;
			break;
		}
	}
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

	logger->fp = fopen(full_path, "w+");
	if (!logger->fp)
	{
		pthread_mutex_destroy(&logger->lock);
		return -1;
	}

	snprintf(logger->log_dir, sizeof(logger->log_dir), "%s", log_dir);
	snprintf(logger->current_path, sizeof(logger->current_path), "%s", full_path);
	logger->next_maintenance_ms = 0;
	logger->cyclic_mode = false;
	logger->cyclic_capacity_bytes = 0;
	logger->cyclic_write_offset = 0;

	setvbuf(logger->fp, NULL, _IOLBF, 0);

	pthread_mutex_lock(&logger->lock);
	maybe_run_maintenance_locked(logger);
	pthread_mutex_unlock(&logger->lock);

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
	maybe_run_maintenance_locked(logger);
	if (!logger->cyclic_mode)
	{
		(void)write_normal_line_locked(logger, kind, payload, len);
	}
	else
	{
		char header[128];
		int n = snprintf(header, sizeof(header), "%llu,%s,", (unsigned long long)realtime_ns(), kind);
		if (n > 0)
		{
			(void)write_cyclic_bytes_locked(logger, header, (size_t)n);
			if (payload && len > 0)
			{
				(void)write_cyclic_bytes_locked(logger, (const char *)payload, len);
			}
			(void)write_cyclic_bytes_locked(logger, "\n", 1);
		}
	}
	pthread_mutex_unlock(&logger->lock);
}

void logger_printf(logger_t *logger, const char *kind, const char *fmt, ...)
{
	if (!logger || !logger->enabled || !logger->fp || !kind || !fmt)
	{
		return;
	}

	pthread_mutex_lock(&logger->lock);
	maybe_run_maintenance_locked(logger);
	if (!logger->cyclic_mode)
	{
		if (fseeko(logger->fp, 0, SEEK_END) == 0)
		{
			fprintf(logger->fp, "%llu,%s,", (unsigned long long)realtime_ns(), kind);

			va_list ap;
			va_start(ap, fmt);
			vfprintf(logger->fp, fmt, ap);
			va_end(ap);

			fputc('\n', logger->fp);
		}
	}
	else
	{
		char header[128];
		int h = snprintf(header, sizeof(header), "%llu,%s,", (unsigned long long)realtime_ns(), kind);
		if (h > 0)
		{
			(void)write_cyclic_bytes_locked(logger, header, (size_t)h);

			va_list ap;
			va_start(ap, fmt);
			va_list ap_len;
			va_copy(ap_len, ap);
			int msg_len = vsnprintf(NULL, 0, fmt, ap_len);
			va_end(ap_len);

			if (msg_len > 0)
			{
				char *msg = (char *)malloc((size_t)msg_len + 1U);
				if (msg)
				{
					(void)vsnprintf(msg, (size_t)msg_len + 1U, fmt, ap);
					(void)write_cyclic_bytes_locked(logger, msg, (size_t)msg_len);
					free(msg);
				}
			}
			va_end(ap);

			(void)write_cyclic_bytes_locked(logger, "\n", 1);
		}
	}
	pthread_mutex_unlock(&logger->lock);
}

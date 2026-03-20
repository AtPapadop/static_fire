#define _POSIX_C_SOURCE 200809L

#include "app/config.h"

#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void app_config_init_defaults(app_config_t *cfg)
{
	if (!cfg)
	{
		return;
	}

	memset(cfg, 0, sizeof(*cfg));
	cfg->ws_port = 8080;
	cfg->max_clients = 128;
	cfg->read_only = false;
	cfg->logging_enabled = false;
	cfg->auto_config_can = true;
	cfg->tick_interval_ms = 5;
	cfg->can_bitrate = 500000u;
	snprintf(cfg->can_ifname, sizeof(cfg->can_ifname), "can0");
	snprintf(cfg->log_path, sizeof(cfg->log_path), "/usr/local/data/ws_can_bridge.csv");
}

static void print_usage_and_exit(const char *argv0)
{
	fprintf(stderr,
					"Usage: %s [--config PATH] [--port N] [--max-clients N] [--read-only] [--logging] "
					"[--can-if can0] [--can-bitrate N] [--no-can-config] "
					"[--tick-ms N] [--log-path PATH]\n",
					argv0);
	exit(EXIT_FAILURE);
}

static char *trim_whitespace(char *s)
{
	if (!s)
	{
		return s;
	}

	while (*s != '\0' && isspace((unsigned char)*s))
	{
		++s;
	}

	char *end = s + strlen(s);
	while (end > s && isspace((unsigned char)*(end - 1)))
	{
		--end;
	}
	*end = '\0';

	return s;
}

static int parse_bool_value(const char *value, bool *out)
{
	if (!value || !out)
	{
		return -1;
	}

	if (strcasecmp(value, "1") == 0 || strcasecmp(value, "true") == 0 ||
		strcasecmp(value, "yes") == 0 || strcasecmp(value, "on") == 0 || strcasecmp(value, "enable") == 0)
	{
		*out = true;
		return 0;
	}

	if (strcasecmp(value, "0") == 0 || strcasecmp(value, "false") == 0 ||
		strcasecmp(value, "no") == 0 || strcasecmp(value, "off") == 0 || strcasecmp(value, "disable") == 0)
	{
		*out = false;
		return 0;
	}

	return -1;
}

static int apply_config_kv(app_config_t *cfg, const char *key, const char *value, const char *config_path, int line_no)
{
	char *endptr = NULL;

	if (strcmp(key, "ws_port") == 0 || strcmp(key, "port") == 0)
	{
		unsigned long v = strtoul(value, &endptr, 10);
		if (*value == '\0' || *endptr != '\0' || v == 0 || v > 65535UL)
		{
			fprintf(stderr, "%s:%d invalid ws_port value: %s\n", config_path, line_no, value);
			return -1;
		}
		cfg->ws_port = (uint16_t)v;
	}
	else if (strcmp(key, "max_clients") == 0)
	{
		long v = strtol(value, &endptr, 10);
		if (*value == '\0' || *endptr != '\0' || v <= 0)
		{
			fprintf(stderr, "%s:%d invalid max_clients value: %s\n", config_path, line_no, value);
			return -1;
		}
		cfg->max_clients = (int)v;
	}
	else if (strcmp(key, "read_only") == 0)
	{
		if (parse_bool_value(value, &cfg->read_only) != 0)
		{
			fprintf(stderr, "%s:%d invalid read_only value: %s\n", config_path, line_no, value);
			return -1;
		}
	}
	else if (strcmp(key, "logging_enabled") == 0 || strcmp(key, "logging") == 0)
	{
		if (parse_bool_value(value, &cfg->logging_enabled) != 0)
		{
			fprintf(stderr, "%s:%d invalid logging value: %s\n", config_path, line_no, value);
			return -1;
		}
	}
	else if (strcmp(key, "auto_config_can") == 0)
	{
		if (parse_bool_value(value, &cfg->auto_config_can) != 0)
		{
			fprintf(stderr, "%s:%d invalid auto_config_can value: %s\n", config_path, line_no, value);
			return -1;
		}
	}
	else if (strcmp(key, "tick_interval_ms") == 0 || strcmp(key, "tick_ms") == 0)
	{
		long v = strtol(value, &endptr, 10);
		if (*value == '\0' || *endptr != '\0' || v <= 0)
		{
			fprintf(stderr, "%s:%d invalid tick_interval_ms value: %s\n", config_path, line_no, value);
			return -1;
		}
		cfg->tick_interval_ms = (int)v;
	}
	else if (strcmp(key, "can_bitrate") == 0)
	{
		unsigned long v = strtoul(value, &endptr, 10);
		if (*value == '\0' || *endptr != '\0' || v == 0)
		{
			fprintf(stderr, "%s:%d invalid can_bitrate value: %s\n", config_path, line_no, value);
			return -1;
		}
		cfg->can_bitrate = (uint32_t)v;
	}
	else if (strcmp(key, "can_ifname") == 0 || strcmp(key, "can_if") == 0)
	{
		snprintf(cfg->can_ifname, sizeof(cfg->can_ifname), "%s", value);
	}
	else if (strcmp(key, "log_path") == 0)
	{
		snprintf(cfg->log_path, sizeof(cfg->log_path), "%s", value);
	}
	else
	{
		fprintf(stderr, "%s:%d unknown key: %s\n", config_path, line_no, key);
		return -1;
	}

	return 0;
}

static int app_config_load_file(app_config_t *cfg, const char *config_path)
{
	FILE *fp = fopen(config_path, "r");
	if (!fp)
	{
		return -1;
	}

	char line[512];
	int line_no = 0;
	while (fgets(line, sizeof(line), fp) != NULL)
	{
		++line_no;
		char *trimmed = trim_whitespace(line);

		if (*trimmed == '\0' || *trimmed == '#' || *trimmed == ';')
		{
			continue;
		}

		char *eq = strchr(trimmed, '=');
		if (!eq)
		{
			fprintf(stderr, "%s:%d expected key=value\n", config_path, line_no);
			errno = EINVAL;
			fclose(fp);
			return -1;
		}

		*eq = '\0';
		char *key = trim_whitespace(trimmed);
		char *value = trim_whitespace(eq + 1);

		if (*value == '"')
		{
			size_t len = strlen(value);
			if (len >= 2 && value[len - 1] == '"')
			{
				value[len - 1] = '\0';
				++value;
			}
		}

		if (apply_config_kv(cfg, key, value, config_path, line_no) != 0)
		{
			errno = EINVAL;
			fclose(fp);
			return -1;
		}
	}

	if (ferror(fp))
	{
		errno = EIO;
		fclose(fp);
		return -1;
	}

	fclose(fp);
	return 0;
}

int app_config_parse(app_config_t *cfg, int argc, char **argv)
{
	if (!cfg)
	{
		return -1;
	}

	const char *config_path = APP_DEFAULT_CONFIG_PATH;
	bool user_provided_config_path = false;

	for (int i = 1; i < argc; ++i)
	{
		if (strcmp(argv[i], "--config") == 0)
		{
			if (i + 1 >= argc)
			{
				print_usage_and_exit(argv[0]);
			}
			config_path = argv[++i];
			user_provided_config_path = true;
		}
	}

	if (app_config_load_file(cfg, config_path) != 0)
	{
		if (user_provided_config_path || errno != ENOENT)
		{
			if (errno == EINVAL)
			{
				fprintf(stderr, "Invalid config file: %s\n", config_path);
			}
			else
			{
				perror(config_path);
			}
			return -1;
		}
	}

	for (int i = 1; i < argc; ++i)
	{
		if (strcmp(argv[i], "--config") == 0)
		{
			if (i + 1 >= argc)
			{
				print_usage_and_exit(argv[0]);
			}
			++i;
		}
		else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
		{
			cfg->ws_port = (uint16_t)atoi(argv[++i]);
		}
		else if (strcmp(argv[i], "--max-clients") == 0 && i + 1 < argc)
		{
			cfg->max_clients = atoi(argv[++i]);
		}
		else if (strcmp(argv[i], "--read-only") == 0)
		{
			cfg->read_only = true;
		}
		else if (strcmp(argv[i], "--logging") == 0)
		{
			cfg->logging_enabled = true;
		}
		else if (strcmp(argv[i], "--can-if") == 0 && i + 1 < argc)
		{
			snprintf(cfg->can_ifname, sizeof(cfg->can_ifname), "%s", argv[++i]);
		}
		else if (strcmp(argv[i], "--can-bitrate") == 0 && i + 1 < argc)
		{
			cfg->can_bitrate = (uint32_t)strtoul(argv[++i], NULL, 10);
		}
		else if (strcmp(argv[i], "--no-can-config") == 0)
		{
			cfg->auto_config_can = false;
		}
		else if (strcmp(argv[i], "--tick-ms") == 0 && i + 1 < argc)
		{
			cfg->tick_interval_ms = atoi(argv[++i]);
		}
		else if (strcmp(argv[i], "--log-path") == 0 && i + 1 < argc)
		{
			snprintf(cfg->log_path, sizeof(cfg->log_path), "%s", argv[++i]);
		}
		else
		{
			print_usage_and_exit(argv[0]);
		}
	}

	if (cfg->ws_port == 0 || cfg->max_clients <= 0 || cfg->tick_interval_ms <= 0 || cfg->can_bitrate == 0)
	{
		return -1;
	}

	return 0;
}

void app_config_print(const app_config_t *cfg)
{
	if (!cfg)
	{
		return;
	}

	printf("WebSocket port: %u\n", cfg->ws_port);
	printf("Max clients: %d\n", cfg->max_clients);
	printf("Read only: %s\n", cfg->read_only ? "yes" : "no");
	printf("Logging: %s\n", cfg->logging_enabled ? "yes" : "no");
	printf("CAN interface: %s\n", cfg->can_ifname);
	printf("CAN bitrate: %u\n", cfg->can_bitrate);
	printf("CAN auto-config: %s\n", cfg->auto_config_can ? "yes" : "no");
	printf("Tick interval: %d ms\n", cfg->tick_interval_ms);
	if (cfg->logging_enabled)
	{
		printf("Log path: %s\n", cfg->log_path);
	}
}

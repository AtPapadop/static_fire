#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include <net/if.h>

typedef struct {
	uint16_t ws_port;
	int max_clients;
	bool read_only;
	bool logging_enabled;
	int tick_interval_ms;
	char can_ifname[IF_NAMESIZE];
	char log_path[256];
} app_config_t;

void app_config_init_defaults(app_config_t *cfg);
int app_config_parse(app_config_t *cfg, int argc, char **argv);
void app_config_print(const app_config_t *cfg);

#endif

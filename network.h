#ifndef NETWORK_H
#define NETWORK_H

#include <stdbool.h>
#include "wizchip_conf.h"
#include "config.h"

bool network_init(wiz_NetInfo *info);
void network_task(void);
bool network_link_is_up(void);
void network_apply_config(const app_config_t *config);
void network_get_info(wiz_NetInfo *info);

#endif

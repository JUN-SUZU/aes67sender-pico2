#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#define CONFIG_STREAM_NAME_LENGTH 32u

typedef struct
{
    uint8_t ip[4];
    uint8_t mask[4];
    uint8_t gateway[4];
    uint8_t dns[4];
    bool multicast_auto;
    uint8_t multicast[4];
    uint16_t rtp_port;
    uint8_t ptp_domain;
    char stream_name[CONFIG_STREAM_NAME_LENGTH];
} app_config_t;

void config_init(void);
const app_config_t *config_get(void);
const app_config_t *config_get_applied(void);
app_config_t *config_edit(void);
void config_set_defaults(app_config_t *value);
void config_resolve_multicast(const app_config_t *value, uint8_t result[4]);
bool config_save(void);
bool config_loaded_from_flash(void);
bool config_is_dirty(void);
void config_mark_dirty(void);
void config_mark_applied(void);
bool config_applied_differs(void);

#endif

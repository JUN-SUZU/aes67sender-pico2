#include "config.h"

#include <stddef.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

#define CONFIG_MAGIC 0x43373641u /* "A67C" */
#define CONFIG_VERSION 1u
#define CONFIG_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    app_config_t config;
    uint32_t crc32;
} config_record_t;

static app_config_t current_config;
static app_config_t applied_config;
static bool loaded_from_flash;
static bool dirty;

static uint32_t crc32_bytes(const void *data, size_t length)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i)
    {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
        {
            uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

void config_set_defaults(app_config_t *value)
{
    memset(value, 0, sizeof(*value));
    const app_config_t defaults = {
        .ip = {192, 168, 2, 5},
        .mask = {255, 255, 254, 0},
        .gateway = {192, 168, 2, 1},
        .dns = {192, 168, 2, 1},
        .multicast_auto = true,
        .multicast = {239, 69, 2, 5},
        .rtp_port = 5004,
        .ptp_domain = 0,
        .stream_name = "Pico2-AES67"
    };
    *value = defaults;
}

void config_init(void)
{
    const config_record_t *record = (const config_record_t *)(XIP_BASE + CONFIG_FLASH_OFFSET);
    bool valid = record->magic == CONFIG_MAGIC &&
                 record->version == CONFIG_VERSION &&
                 record->size == sizeof(app_config_t) &&
                 record->crc32 == crc32_bytes(&record->config, sizeof(record->config));
    if (valid)
    {
        current_config = record->config;
        loaded_from_flash = true;
    }
    else
    {
        config_set_defaults(&current_config);
        loaded_from_flash = false;
    }
    applied_config = current_config;
    dirty = false;
}

const app_config_t *config_get(void) { return &current_config; }
const app_config_t *config_get_applied(void) { return &applied_config; }
app_config_t *config_edit(void) { return &current_config; }

void config_resolve_multicast(const app_config_t *value, uint8_t result[4])
{
    if (value->multicast_auto)
    {
        result[0] = 239; result[1] = 69;
        result[2] = value->ip[2]; result[3] = value->ip[3];
    }
    else
    {
        memcpy(result, value->multicast, 4);
    }
}

bool config_save(void)
{
    config_record_t record;
    memset(&record, 0xFF, sizeof(record));
    record.magic = CONFIG_MAGIC;
    record.version = CONFIG_VERSION;
    record.size = sizeof(app_config_t);
    record.config = current_config;
    record.crc32 = crc32_bytes(&record.config, sizeof(record.config));

    uint8_t page[FLASH_PAGE_SIZE];
    memset(page, 0xFF, sizeof(page));
    memcpy(page, &record, sizeof(record));

    uint32_t irq_state = save_and_disable_interrupts();
    flash_range_erase(CONFIG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(CONFIG_FLASH_OFFSET, page, sizeof(page));
    restore_interrupts(irq_state);

    loaded_from_flash = true;
    dirty = false;
    return true;
}

bool config_loaded_from_flash(void) { return loaded_from_flash; }
bool config_is_dirty(void) { return dirty; }
void config_mark_dirty(void) { dirty = true; }
void config_mark_applied(void) { applied_config = current_config; }
bool config_applied_differs(void)
{
    return memcmp(&applied_config, &current_config, sizeof(current_config)) != 0;
}

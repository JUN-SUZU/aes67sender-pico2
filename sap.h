#ifndef SAP_H
#define SAP_H

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    bool socket_open;
    uint8_t destination[4];
    uint16_t port;
    uint16_t message_id_hash;
    uint32_t announcements_sent;
    uint32_t send_errors;
    uint32_t sdp_bytes;
    uint64_t last_announcement_us;
} sap_status_t;

void sap_init(const uint8_t mac[6]);
void sap_reconfigure(void);
void sap_task(bool link_up);
void sap_get_status(sap_status_t *status);

#endif

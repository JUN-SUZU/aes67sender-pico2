#ifndef AES67_H
#define AES67_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    AES67_STATE_WAIT_PTP,
    AES67_STATE_WAIT_AUDIO,
    AES67_STATE_PREBUFFER,
    AES67_STATE_STREAMING
} aes67_state_t;

typedef struct
{
    aes67_state_t state;
    uint8_t destination[4];
    uint16_t port;
    uint16_t sequence;
    uint32_t rtp_timestamp;
    uint32_t ssrc;
    uint32_t packets_sent;
    uint32_t send_errors;
    uint32_t underruns;
    uint32_t late_packets;
    uint32_t prebuffer_target;
} aes67_status_t;

void aes67_init(const uint8_t mac[6]);
void aes67_reconfigure(void);
void aes67_task(bool link_up);
void aes67_get_status(aes67_status_t *status);
const char *aes67_state_name(aes67_state_t state);

#endif

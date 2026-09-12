#ifndef PTP_H
#define PTP_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    PTP_STATE_LISTENING,
    PTP_STATE_ACQUIRING,
    PTP_STATE_LOCKED
} ptp_state_t;

typedef struct
{
    ptp_state_t state;
    uint8_t domain;
    uint8_t gm_clock_id[8];
    uint8_t priority1;
    uint8_t priority2;
    uint8_t clock_class;
    uint8_t clock_accuracy;
    uint16_t clock_variance;
    int64_t phase_error_ns;
    int64_t phase_min_ns;
    int64_t phase_max_ns;
    int64_t phase_average_ns;
    int64_t mean_path_delay_ns;
    uint64_t last_announce_us;
    uint64_t last_sync_us;
    uint32_t announce_rx;
    uint32_t sync_rx;
    uint32_t follow_up_rx;
    uint32_t delay_req_tx;
    uint32_t delay_resp_rx;
    uint32_t malformed_rx;
    uint32_t domain_mismatch;
    uint32_t sequence_mismatch;
    uint32_t measurements;
} ptp_status_t;

void ptp_init(const uint8_t mac[6], uint8_t domain);
void ptp_set_domain(uint8_t domain);
void ptp_task(bool link_up);
bool ptp_is_locked(void);
uint64_t ptp_get_time_ns(void);
void ptp_get_status(ptp_status_t *status);
void ptp_reset_phase_statistics(void);
const char *ptp_state_name(ptp_state_t state);

#endif

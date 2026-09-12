#include "ptp.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#include "pico/stdlib.h"
#include "socket.h"
#include "w5500.h"

#define PTP_EVENT_SOCKET 1u
#define PTP_GENERAL_SOCKET 2u
#define PTP_EVENT_PORT 319u
#define PTP_GENERAL_PORT 320u
#define PTP_HEADER_SIZE 34u
#define PTP_ANNOUNCE 0x0Bu
#define PTP_SYNC 0x00u
#define PTP_DELAY_REQ 0x01u
#define PTP_FOLLOW_UP 0x08u
#define PTP_DELAY_RESP 0x09u
#define PTP_TWO_STEP_FLAG 0x0200u
#define PTP_ANNOUNCE_TIMEOUT_US 3000000ull
#define PTP_DELAY_REQUEST_PERIOD_US 1000000ull
#define PTP_LOCK_LIMIT_NS 2000000ll

static const uint8_t ptp_multicast_ip[4] = {224, 0, 1, 129};
static ptp_status_t ptp_status;
static uint8_t local_port_identity[10];
static uint8_t master_port_identity[10];
static bool master_valid;
static bool event_open;
static bool general_open;
static bool have_sync;
static bool have_delay;
static uint16_t sync_sequence;
static uint16_t delay_sequence;
static uint16_t next_delay_sequence = 1;
static int64_t sync_t2_ns;
static int64_t sync_correction_ns;
static int64_t master_t1_ns;
static int64_t delay_t3_ns;
static int64_t master_t4_ns;
static int64_t delay_correction_ns;
static int64_t filtered_offset_ns;
static bool mapping_valid;
static uint32_t consecutive_good;
static uint64_t last_delay_request_us;

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint64_t be64(const uint8_t *p)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value = (value << 8) | p[i];
    return value;
}

static void put_be16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8); p[1] = (uint8_t)value;
}

static int64_t local_time_ns(void)
{
    return (int64_t)time_us_64() * 1000ll;
}

static int64_t timestamp_ns(const uint8_t *p)
{
    uint64_t seconds = ((uint64_t)p[0] << 40) | ((uint64_t)p[1] << 32) |
                       ((uint64_t)p[2] << 24) | ((uint64_t)p[3] << 16) |
                       ((uint64_t)p[4] << 8) | p[5];
    return (int64_t)(seconds * 1000000000ull + be16(p + 6) * 65536ull +
                     (uint16_t)(((uint16_t)p[8] << 8) | p[9]));
}

/* correctionField is signed, scaled nanoseconds (2^-16 ns). */
static int64_t correction_ns(const uint8_t *packet)
{
    return ((int64_t)be64(packet + 8)) / 65536ll;
}

static bool open_multicast_udp(uint8_t sn, uint16_t port)
{
    if (getSn_SR(sn) != SOCK_CLOSED) close(sn);
    uint8_t dipr[4];
    memcpy(dipr, ptp_multicast_ip, sizeof(dipr));
    setSn_DIPR(sn, dipr); /* mutable copy avoids ioLibrary const warning */
    setSn_DPORT(sn, port);
    bool opened = socket(sn, Sn_MR_UDP, port, SF_MULTI_ENABLE) == (int8_t)sn;
    if (opened)
    {
        setSn_TOS(sn, sn == PTP_EVENT_SOCKET ? 0xE0u : 0xC0u);
        setSn_TTL(sn, 32u);
    }
    return opened;
}

static bool valid_header(const uint8_t *packet, uint16_t length)
{
    if (length < PTP_HEADER_SIZE || (packet[1] & 0x0Fu) != 2u ||
        be16(packet + 2) > length)
    {
        ptp_status.malformed_rx++;
        return false;
    }
    if (packet[4] != ptp_status.domain)
    {
        ptp_status.domain_mismatch++;
        return false;
    }
    return true;
}

static void update_measurement(void)
{
    if (!have_sync || !have_delay) return;

    int64_t forward = sync_t2_ns - master_t1_ns - sync_correction_ns;
    int64_t reverse = master_t4_ns - delay_t3_ns - delay_correction_ns;
    int64_t measured_offset = (forward - reverse) / 2ll;
    ptp_status.mean_path_delay_ns = (forward + reverse) / 2ll;

    int64_t phase;
    if (!mapping_valid)
    {
        filtered_offset_ns = measured_offset;
        mapping_valid = true;
        ptp_status.state = PTP_STATE_ACQUIRING;
        have_delay = false;
        return; /* baseline acquisition is not a phase-error sample */
    }
    else
    {
        phase = measured_offset - filtered_offset_ns;
        filtered_offset_ns += phase / 8ll;
    }

    ptp_status.phase_error_ns = phase;
    ptp_status.measurements++;
    if (ptp_status.measurements == 1)
    {
        ptp_status.phase_min_ns = phase;
        ptp_status.phase_max_ns = phase;
        ptp_status.phase_average_ns = phase;
    }
    else
    {
        if (phase < ptp_status.phase_min_ns) ptp_status.phase_min_ns = phase;
        if (phase > ptp_status.phase_max_ns) ptp_status.phase_max_ns = phase;
        ptp_status.phase_average_ns +=
            (phase - ptp_status.phase_average_ns) / (int64_t)ptp_status.measurements;
    }

    if (phase >= -PTP_LOCK_LIMIT_NS && phase <= PTP_LOCK_LIMIT_NS)
    {
        if (consecutive_good < UINT32_MAX) consecutive_good++;
    }
    else
    {
        consecutive_good = 0;
    }
    ptp_status.state = consecutive_good >= 3u ? PTP_STATE_LOCKED : PTP_STATE_ACQUIRING;
    have_delay = false; /* each DelayResp contributes at most one measurement */
}

static void handle_packet(const uint8_t *packet, uint16_t length, int64_t rx_ns)
{
    if (!valid_header(packet, length)) return;
    uint8_t message = packet[0] & 0x0Fu;
    uint16_t sequence = be16(packet + 30);

    if (message == PTP_ANNOUNCE && length >= 64u)
    {
        ptp_status.announce_rx++;
        ptp_status.last_announce_us = time_us_64();
        memcpy(master_port_identity, packet + 20, 10);
        master_valid = true;
        ptp_status.priority1 = packet[47];
        ptp_status.clock_class = packet[48];
        ptp_status.clock_accuracy = packet[49];
        ptp_status.clock_variance = be16(packet + 50);
        ptp_status.priority2 = packet[52];
        memcpy(ptp_status.gm_clock_id, packet + 53, 8);
        if (ptp_status.state == PTP_STATE_LISTENING) ptp_status.state = PTP_STATE_ACQUIRING;
    }
    else if (message == PTP_SYNC && length >= 44u)
    {
        if (master_valid && memcmp(packet + 20, master_port_identity, 10) != 0)
        {
            ptp_status.sequence_mismatch++;
            return;
        }
        ptp_status.sync_rx++;
        ptp_status.last_sync_us = time_us_64();
        sync_sequence = sequence;
        sync_t2_ns = rx_ns;
        sync_correction_ns = correction_ns(packet);
        have_sync = false;
        if ((be16(packet + 6) & PTP_TWO_STEP_FLAG) == 0u)
        {
            master_t1_ns = timestamp_ns(packet + 34);
            have_sync = true;
            update_measurement();
        }
    }
    else if (message == PTP_FOLLOW_UP && length >= 44u)
    {
        ptp_status.follow_up_rx++;
        if (sequence != sync_sequence ||
            (master_valid && memcmp(packet + 20, master_port_identity, 10) != 0))
        {
            ptp_status.sequence_mismatch++;
            return;
        }
        master_t1_ns = timestamp_ns(packet + 34);
        sync_correction_ns += correction_ns(packet);
        have_sync = true;
        update_measurement();
    }
    else if (message == PTP_DELAY_RESP && length >= 54u)
    {
        ptp_status.delay_resp_rx++;
        if (sequence != delay_sequence || memcmp(packet + 44, local_port_identity, 10) != 0)
        {
            ptp_status.sequence_mismatch++;
            return;
        }
        master_t4_ns = timestamp_ns(packet + 34);
        delay_correction_ns = correction_ns(packet);
        have_delay = true;
        update_measurement();
    }
}

static void receive_socket(uint8_t sn)
{
    uint8_t packet[128];
    uint8_t source_ip[4];
    uint16_t source_port = 0;
    uint16_t available = getSn_RX_RSR(sn);
    if (available == 0) return;
    int64_t rx_ns = local_time_ns();
    int32_t length = recvfrom(sn, packet, sizeof(packet), source_ip, &source_port);
    if (length > 0) handle_packet(packet, (uint16_t)length, rx_ns);
}

static void send_delay_request(void)
{
    uint8_t packet[44];
    memset(packet, 0, sizeof(packet));
    packet[0] = PTP_DELAY_REQ;
    packet[1] = 0x02;
    put_be16(packet + 2, sizeof(packet));
    packet[4] = ptp_status.domain;
    memcpy(packet + 20, local_port_identity, 10);
    delay_sequence = next_delay_sequence++;
    put_be16(packet + 30, delay_sequence);
    packet[32] = 1;
    packet[33] = 0;

    int64_t before = local_time_ns();
    int32_t sent = sendto(PTP_EVENT_SOCKET, packet, sizeof(packet),
                          (uint8_t *)ptp_multicast_ip, PTP_EVENT_PORT);
    int64_t after = local_time_ns();
    if (sent == (int32_t)sizeof(packet))
    {
        delay_t3_ns = before + (after - before) / 2ll;
        ptp_status.delay_req_tx++;
    }
}

void ptp_init(const uint8_t mac[6], uint8_t domain)
{
    memset(&ptp_status, 0, sizeof(ptp_status));
    ptp_status.state = PTP_STATE_LISTENING;
    ptp_status.domain = domain;
    local_port_identity[0] = mac[0]; local_port_identity[1] = mac[1];
    local_port_identity[2] = mac[2]; local_port_identity[3] = 0xFF;
    local_port_identity[4] = 0xFE; local_port_identity[5] = mac[3];
    local_port_identity[6] = mac[4]; local_port_identity[7] = mac[5];
    put_be16(local_port_identity + 8, 1);
}

void ptp_set_domain(uint8_t domain)
{
    if (domain == ptp_status.domain) return;
    ptp_status.domain = domain;
    ptp_status.state = PTP_STATE_LISTENING;
    master_valid = false; mapping_valid = false; have_sync = false; have_delay = false;
    consecutive_good = 0;
}

void ptp_task(bool link_up)
{
    if (!link_up)
    {
        ptp_status.state = PTP_STATE_LISTENING;
        event_open = general_open = false;
        master_valid = mapping_valid = false;
        consecutive_good = 0;
        return;
    }
    if (!event_open || getSn_SR(PTP_EVENT_SOCKET) != SOCK_UDP)
        event_open = open_multicast_udp(PTP_EVENT_SOCKET, PTP_EVENT_PORT);
    if (!general_open || getSn_SR(PTP_GENERAL_SOCKET) != SOCK_UDP)
        general_open = open_multicast_udp(PTP_GENERAL_SOCKET, PTP_GENERAL_PORT);
    if (!event_open || !general_open) return;

    receive_socket(PTP_EVENT_SOCKET);
    receive_socket(PTP_GENERAL_SOCKET);

    uint64_t now = time_us_64();
    if (ptp_status.last_announce_us != 0 && now - ptp_status.last_announce_us > PTP_ANNOUNCE_TIMEOUT_US)
    {
        ptp_status.state = PTP_STATE_LISTENING;
        master_valid = mapping_valid = false;
        consecutive_good = 0;
    }
    if (master_valid && now - last_delay_request_us >= PTP_DELAY_REQUEST_PERIOD_US)
    {
        last_delay_request_us = now;
        send_delay_request();
    }
}

bool ptp_is_locked(void) { return ptp_status.state == PTP_STATE_LOCKED && mapping_valid; }

uint64_t ptp_get_time_ns(void)
{
    if (!mapping_valid) return 0;
    int64_t value = local_time_ns() - filtered_offset_ns;
    return value > 0 ? (uint64_t)value : 0;
}

void ptp_get_status(ptp_status_t *status) { if (status) *status = ptp_status; }

void ptp_reset_phase_statistics(void)
{
    ptp_status.phase_min_ns = ptp_status.phase_error_ns;
    ptp_status.phase_max_ns = ptp_status.phase_error_ns;
    ptp_status.phase_average_ns = ptp_status.phase_error_ns;
    ptp_status.measurements = 0;
}

const char *ptp_state_name(ptp_state_t state)
{
    switch (state)
    {
        case PTP_STATE_ACQUIRING: return "ACQUIRING";
        case PTP_STATE_LOCKED: return "LOCKED";
        default: return "LISTENING";
    }
}

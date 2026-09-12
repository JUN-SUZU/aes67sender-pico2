#include "sap.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "aes67.h"
#include "config.h"
#include "network.h"
#include "ptp.h"
#include "pico/stdlib.h"
#include "socket.h"
#include "w5500.h"

#define SAP_SOCKET 4u
#define SAP_PORT 9875u
#define SAP_INTERVAL_US 30000000ull
#define SAP_HEADER_BYTES 8u
#define SAP_PACKET_MAX 768u
#define SAP_SDP_MAX 640u

static const uint8_t sap_multicast[4] = {239, 255, 255, 255};
static const char sap_payload_type[] = "application/sdp";
static sap_status_t sap_status;
static uint32_t session_id;
static uint32_t session_version = 1;
static bool send_immediately;
static uint8_t advertised_gm_id[8];
static bool advertised_gm_valid;

static uint16_t sap_hash(const uint8_t *data, size_t length)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < length; ++i)
    {
        hash ^= data[i];
        hash *= 16777619u;
    }
    uint16_t result = (uint16_t)(hash ^ (hash >> 16));
    return result == 0 ? 1u : result;
}

static bool ensure_socket(void)
{
    if (sap_status.socket_open && getSn_SR(SAP_SOCKET) == SOCK_UDP) return true;
    if (getSn_SR(SAP_SOCKET) != SOCK_CLOSED) close(SAP_SOCKET);

    /* Sn_MR_MULTIによりEthernet宛先を01:00:5e:7f:ff:ffにする。 */
    uint8_t multicast[4];
    memcpy(multicast, sap_status.destination, sizeof(multicast));
    setSn_DIPR(SAP_SOCKET, multicast);
    setSn_DPORT(SAP_SOCKET, sap_status.port);
    sap_status.socket_open =
        socket(SAP_SOCKET, Sn_MR_UDP, SAP_PORT, SF_MULTI_ENABLE) == (int8_t)SAP_SOCKET;
    if (sap_status.socket_open)
    {
        setSn_TTL(SAP_SOCKET, 255u);
    }
    return sap_status.socket_open;
}

static int make_sdp(char *sdp, size_t capacity)
{
    const app_config_t *cfg = config_get_applied();
    aes67_status_t aes;
    ptp_status_t ptp;
    wiz_NetInfo net;
    aes67_get_status(&aes);
    ptp_get_status(&ptp);
    network_get_info(&net);

    return snprintf(
        sdp, capacity,
        "v=0\r\n"
        "o=- %" PRIu32 " %" PRIu32 " IN IP4 %u.%u.%u.%u\r\n"
        "s=%s\r\n"
        "c=IN IP4 %u.%u.%u.%u/32\r\n"
        "t=0 0\r\n"
        "m=audio %u RTP/AVP 96\r\n"
        "a=rtpmap:96 L24/48000/2\r\n"
        "a=ptime:1\r\n"
        "a=maxptime:1\r\n"
        "a=sendonly\r\n"
        "a=ts-refclk:ptp=IEEE1588-2008:%02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X:%u\r\n"
        "a=mediaclk:direct=0\r\n",
        session_id, session_version,
        net.ip[0], net.ip[1], net.ip[2], net.ip[3],
        cfg->stream_name,
        aes.destination[0], aes.destination[1], aes.destination[2], aes.destination[3],
        aes.port,
        ptp.gm_clock_id[0], ptp.gm_clock_id[1], ptp.gm_clock_id[2], ptp.gm_clock_id[3],
        ptp.gm_clock_id[4], ptp.gm_clock_id[5], ptp.gm_clock_id[6], ptp.gm_clock_id[7],
        ptp.domain);
}

static void send_announcement(void)
{
    uint8_t packet[SAP_PACKET_MAX];
    char sdp[SAP_SDP_MAX];
    ptp_status_t ptp;
    ptp_get_status(&ptp);
    if (advertised_gm_valid && memcmp(advertised_gm_id, ptp.gm_clock_id, 8) != 0)
    {
        session_version++;
        if (session_version == 0) session_version = 1;
    }
    memcpy(advertised_gm_id, ptp.gm_clock_id, 8);
    advertised_gm_valid = true;
    int sdp_length = make_sdp(sdp, sizeof(sdp));
    if (sdp_length <= 0 || (size_t)sdp_length >= sizeof(sdp))
    {
        sap_status.send_errors++;
        return;
    }

    size_t payload_type_bytes = sizeof(sap_payload_type); /* includes NUL */
    size_t packet_length = SAP_HEADER_BYTES + payload_type_bytes + (size_t)sdp_length;
    if (packet_length > sizeof(packet))
    {
        sap_status.send_errors++;
        return;
    }

    wiz_NetInfo net;
    network_get_info(&net);
    packet[0] = 0x20u; /* V=1, IPv4, announcement, plain, uncompressed */
    packet[1] = 0u;    /* no authentication words */
    sap_status.message_id_hash = sap_hash((const uint8_t *)sdp, (size_t)sdp_length);
    packet[2] = (uint8_t)(sap_status.message_id_hash >> 8);
    packet[3] = (uint8_t)sap_status.message_id_hash;
    memcpy(packet + 4, net.ip, 4);
    memcpy(packet + SAP_HEADER_BYTES, sap_payload_type, payload_type_bytes);
    memcpy(packet + SAP_HEADER_BYTES + payload_type_bytes, sdp, (size_t)sdp_length);

    int32_t sent = sendto(SAP_SOCKET, packet, (uint16_t)packet_length,
                          sap_status.destination, sap_status.port);
    if (sent == (int32_t)packet_length)
    {
        sap_status.announcements_sent++;
        sap_status.sdp_bytes = (uint32_t)sdp_length;
        sap_status.last_announcement_us = time_us_64();
    }
    else
    {
        sap_status.send_errors++;
    }
}

void sap_init(const uint8_t mac[6])
{
    memset(&sap_status, 0, sizeof(sap_status));
    memcpy(sap_status.destination, sap_multicast, sizeof(sap_multicast));
    sap_status.port = SAP_PORT;
    session_id = 2166136261u;
    for (unsigned i = 0; i < 6; ++i)
    {
        session_id ^= mac[i];
        session_id *= 16777619u;
    }
    if (session_id == 0) session_id = 1;
    send_immediately = true;
    advertised_gm_valid = false;
}

void sap_reconfigure(void)
{
    session_version++;
    if (session_version == 0) session_version = 1;
    send_immediately = true;
}

void sap_task(bool link_up)
{
    aes67_status_t aes;
    aes67_get_status(&aes);
    if (!link_up || aes.state != AES67_STATE_STREAMING)
    {
        send_immediately = true;
        return;
    }
    if (!ensure_socket())
    {
        sap_status.send_errors++;
        return;
    }
    uint64_t now = time_us_64();
    if (send_immediately || sap_status.last_announcement_us == 0 ||
        now - sap_status.last_announcement_us >= SAP_INTERVAL_US)
    {
        send_immediately = false;
        send_announcement();
    }
}

void sap_get_status(sap_status_t *status)
{
    if (status != NULL) *status = sap_status;
}

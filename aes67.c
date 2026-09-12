#include "aes67.h"

#include <string.h>

#include "audiorcv.h"
#include "config.h"
#include "ptp.h"
#include "pico/stdlib.h"
#include "socket.h"
#include "w5500.h"

#define AES67_SOCKET 3u
#define AES67_PAYLOAD_TYPE 96u
#define AES67_CHANNELS 2u
#define AES67_FRAMES_PER_PACKET 48u
#define AES67_PACKET_PERIOD_NS 1000000ull
#define AES67_PREBUFFER_FRAMES 192u
#define AES67_RTP_HEADER_BYTES 12u
#define AES67_PAYLOAD_BYTES (AES67_FRAMES_PER_PACKET * AES67_CHANNELS * 3u)
#define AES67_PACKET_BYTES (AES67_RTP_HEADER_BYTES + AES67_PAYLOAD_BYTES)

static aes67_status_t status;
static bool socket_open;
static bool marker_pending;
static uint64_t next_packet_time_ns;

static void put_be16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8); p[1] = (uint8_t)value;
}

static void put_be32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24); p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8); p[3] = (uint8_t)value;
}

static uint32_t timestamp_for_ptp_time(uint64_t ns)
{
    uint64_t seconds = ns / 1000000000ull;
    uint64_t remainder = ns % 1000000000ull;
    return (uint32_t)(seconds * 48000ull + remainder * 48000ull / 1000000000ull);
}

static void release_audio(void)
{
    audiorcv_set_network_consumer(false);
    status.state = AES67_STATE_WAIT_PTP;
    next_packet_time_ns = 0;
    marker_pending = true;
}

static bool ensure_socket(void)
{
    if (socket_open && getSn_SR(AES67_SOCKET) == SOCK_UDP) return true;
    if (getSn_SR(AES67_SOCKET) != SOCK_CLOSED) close(AES67_SOCKET);

    /*
     * W5500は通常UDP socketだとmulticast IPをgateway経由のunicast MACへ
     * 解決することがある。OPEN前に宛先を設定しSn_MR_MULTIを有効にする。
     */
    uint8_t multicast[4];
    memcpy(multicast, status.destination, sizeof(multicast));
    setSn_DIPR(AES67_SOCKET, multicast);
    setSn_DPORT(AES67_SOCKET, status.port);
    socket_open = socket(AES67_SOCKET, Sn_MR_UDP, status.port, SF_MULTI_ENABLE) == (int8_t)AES67_SOCKET;
    if (socket_open)
    {
        setSn_TOS(AES67_SOCKET, 0xB8u); /* DSCP EF (46), ECN 0 */
        setSn_TTL(AES67_SOCKET, 32u);
    }
    return socket_open;
}

void aes67_init(const uint8_t mac[6])
{
    memset(&status, 0, sizeof(status));
    status.state = AES67_STATE_WAIT_PTP;
    status.prebuffer_target = AES67_PREBUFFER_FRAMES;
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < 6; ++i) { hash ^= mac[i]; hash *= 16777619u; }
    status.ssrc = hash;
    status.sequence = (uint16_t)time_us_64();
    marker_pending = true;
    aes67_reconfigure();
}

void aes67_reconfigure(void)
{
    const app_config_t *cfg = config_get();
    config_resolve_multicast(cfg, status.destination);
    status.port = cfg->rtp_port;
    if (socket_open)
    {
        close(AES67_SOCKET);
        socket_open = false;
    }
    release_audio();
}

void aes67_task(bool link_up)
{
    if (!link_up || !ptp_is_locked())
    {
        release_audio();
        return;
    }
    if (!audiorcv_usb_streaming() || audiorcv_sample_rate() != 48000u)
    {
        audiorcv_set_network_consumer(false);
        status.state = AES67_STATE_WAIT_AUDIO;
        next_packet_time_ns = 0;
        marker_pending = true;
        return;
    }
    if (!ensure_socket())
    {
        status.send_errors++;
        return;
    }

    audiorcv_set_network_consumer(true);
    if (status.state != AES67_STATE_STREAMING)
    {
        status.state = AES67_STATE_PREBUFFER;
        if (audiorcv_ring_count() < AES67_PREBUFFER_FRAMES) return;

        uint64_t now = ptp_get_time_ns();
        next_packet_time_ns = ((now / AES67_PACKET_PERIOD_NS) + 1ull) * AES67_PACKET_PERIOD_NS;
        status.rtp_timestamp = timestamp_for_ptp_time(next_packet_time_ns);
        status.state = AES67_STATE_STREAMING;
        marker_pending = true;
        return;
    }

    uint64_t now = ptp_get_time_ns();
    if (now < next_packet_time_ns) return;
    if (now - next_packet_time_ns > 2000000ull)
    {
        status.late_packets++;
        next_packet_time_ns = ((now / AES67_PACKET_PERIOD_NS) + 1ull) * AES67_PACKET_PERIOD_NS;
        status.rtp_timestamp = timestamp_for_ptp_time(next_packet_time_ns);
        marker_pending = true;
        return;
    }
    if (audiorcv_ring_count() < AES67_FRAMES_PER_PACKET)
    {
        status.underruns++;
        status.state = AES67_STATE_PREBUFFER;
        next_packet_time_ns = 0;
        marker_pending = true;
        return;
    }

    audio_stereo_frame_t frames[AES67_FRAMES_PER_PACKET];
    if (audiorcv_read_frames(frames, AES67_FRAMES_PER_PACKET) != AES67_FRAMES_PER_PACKET)
    {
        status.underruns++;
        status.state = AES67_STATE_PREBUFFER;
        return;
    }

    uint8_t packet[AES67_PACKET_BYTES];
    packet[0] = 0x80;
    packet[1] = AES67_PAYLOAD_TYPE | (marker_pending ? 0x80u : 0u);
    put_be16(packet + 2, status.sequence);
    put_be32(packet + 4, status.rtp_timestamp);
    put_be32(packet + 8, status.ssrc);
    size_t out = AES67_RTP_HEADER_BYTES;
    for (unsigned i = 0; i < AES67_FRAMES_PER_PACKET; ++i)
    {
        uint16_t left = (uint16_t)frames[i].left;
        uint16_t right = (uint16_t)frames[i].right;
        packet[out++] = (uint8_t)(left >> 8); packet[out++] = (uint8_t)left; packet[out++] = 0;
        packet[out++] = (uint8_t)(right >> 8); packet[out++] = (uint8_t)right; packet[out++] = 0;
    }

    int32_t sent = sendto(AES67_SOCKET, packet, sizeof(packet), status.destination, status.port);
    if (sent == (int32_t)sizeof(packet))
    {
        status.packets_sent++;
        status.sequence++;
        status.rtp_timestamp += AES67_FRAMES_PER_PACKET;
        marker_pending = false;
    }
    else
    {
        status.send_errors++;
        marker_pending = true;
    }
    next_packet_time_ns += AES67_PACKET_PERIOD_NS;
}

void aes67_get_status(aes67_status_t *result) { if (result) *result = status; }

const char *aes67_state_name(aes67_state_t state)
{
    switch (state)
    {
        case AES67_STATE_WAIT_AUDIO: return "WAIT AUDIO";
        case AES67_STATE_PREBUFFER: return "PREBUFFER";
        case AES67_STATE_STREAMING: return "STREAMING";
        default: return "WAIT PTP";
    }
}

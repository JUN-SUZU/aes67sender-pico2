#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "wizchip_conf.h"

#include "aes67.h"
#include "audiorcv.h"
#include "cli.h"
#include "config.h"
#include "network.h"
#include "ptp.h"
#include "sap.h"
#include "terminal.h"
#include "tui.h"

static bool tui_active;
static uint64_t last_tui_draw_us;

static void print_ip(const uint8_t value[4])
{
    terminal_printf("%u.%u.%u.%u", value[0], value[1], value[2], value[3]);
}

static bool parse_ip(const char *text, uint8_t result[4])
{
    unsigned a, b, c, d;
    char tail;
    if (sscanf(text, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 ||
        a > 255 || b > 255 || c > 255 || d > 255) return false;
    result[0] = (uint8_t)a; result[1] = (uint8_t)b;
    result[2] = (uint8_t)c; result[3] = (uint8_t)d;
    return true;
}

static bool parse_u32(const char *text, uint32_t maximum, uint32_t *result)
{
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (text[0] == '\0' || end == NULL || *end != '\0' || value > maximum) return false;
    *result = (uint32_t)value;
    return true;
}

static void show_config_value(const app_config_t *cfg, const char *title)
{
    uint8_t multicast[4];
    config_resolve_multicast(cfg, multicast);
    terminal_printf("%s\r\n  IP        : ", title); print_ip(cfg->ip);
    terminal_printf("\r\n  Mask      : "); print_ip(cfg->mask);
    terminal_printf("\r\n  Gateway   : "); print_ip(cfg->gateway);
    terminal_printf("\r\n  DNS       : "); print_ip(cfg->dns);
    terminal_printf("\r\n  Multicast : %s ", cfg->multicast_auto ? "AUTO" : "MANUAL"); print_ip(multicast);
    terminal_printf("\r\n  RTP port  : %u\r\n  PTP domain: %u\r\n  Stream    : %s\r\n",
                    cfg->rtp_port, cfg->ptp_domain, cfg->stream_name);
}

static void show_ptp(void)
{
    ptp_status_t s;
    ptp_get_status(&s);
    terminal_printf("PTP %s domain=%u phase=%+" PRId64 " ns min=%+" PRId64
                    " ns max=%+" PRId64 " ns avg=%+" PRId64 " ns samples=%" PRIu32 "\r\n",
                    ptp_state_name(s.state), s.domain, s.phase_error_ns,
                    s.phase_min_ns, s.phase_max_ns, s.phase_average_ns, s.measurements);
    terminal_printf("delay=%+" PRId64 " ns announce=%" PRIu32 " sync=%" PRIu32
                    " followUp=%" PRIu32 " delayReq=%" PRIu32 " delayResp=%" PRIu32 "\r\n",
                    s.mean_path_delay_ns, s.announce_rx, s.sync_rx, s.follow_up_rx,
                    s.delay_req_tx, s.delay_resp_rx);
}

static void show_aes67(void)
{
    aes67_status_t s;
    aes67_get_status(&s);
    terminal_printf("AES67 %s destination=", aes67_state_name(s.state));
    print_ip(s.destination);
    terminal_printf(":%u PT=96 L24 stereo 48kHz 1ms packets=%" PRIu32
                    " errors=%" PRIu32 " underruns=%" PRIu32 " late=%" PRIu32 "\r\n",
                    s.port, s.packets_sent, s.send_errors, s.underruns, s.late_packets);
}

static void show_sap(void)
{
    sap_status_t s;
    sap_get_status(&s);
    terminal_printf("SAP %s destination=%u.%u.%u.%u:%u announcements=%" PRIu32
                    " errors=%" PRIu32 " hash=0x%04X SDP=%" PRIu32 " bytes\r\n",
                    s.socket_open ? "READY" : "WAITING",
                    s.destination[0], s.destination[1], s.destination[2], s.destination[3], s.port,
                    s.announcements_sent, s.send_errors, s.message_id_hash, s.sdp_bytes);
}

static void tui_input(uint8_t ch)
{
    if (ch == 'q' || ch == 'Q' || ch == 0x1B)
    {
        tui_active = false;
        tui_end();
        cli_enable();
    }
}

static void tui_signed_us(const char *label, int64_t ns)
{
    int64_t absolute = ns < 0 ? -ns : ns;
    tui_printf(" %-17s: %c%" PRId64 ".%03" PRId64 " us\r\n",
               label, ns < 0 ? '-' : '+', absolute / 1000, absolute % 1000);
}

static void draw_tui(void)
{
    ptp_status_t p;
    aes67_status_t a;
    sap_status_t sap;
    wiz_NetInfo net;
    ptp_get_status(&p);
    aes67_get_status(&a);
    sap_get_status(&sap);
    network_get_info(&net);
    uint64_t now_us = time_us_64();
    uint64_t ptp_ns = ptp_get_time_ns();

    tui_home();
    tui_set_bold(true); tui_write(" Pico 2 AES67 / PTPv2 Monitor\r\n"); tui_set_bold(false);
    tui_write(" q / ESC: return to CLI\r\n\r\n");
    tui_printf(" Ethernet         : %-10s  CDC: %s\r\n",
               network_link_is_up() ? "UP" : "DOWN", terminal_connected() ? "CONNECTED" : "DISCONNECTED");
    tui_printf(" Applied IP       : %u.%u.%u.%u  config:%s dirty:%s pending:%s\r\n",
               net.ip[0], net.ip[1], net.ip[2], net.ip[3],
               config_loaded_from_flash() ? "FLASH" : "DEFAULT",
               config_is_dirty() ? "YES" : "NO",
               config_applied_differs() ? "YES" : "NO");
    tui_printf(" USB Audio        : %-10s  %s  %" PRIu32 " Hz\r\n",
               audiorcv_usb_mounted() ? "MOUNTED" : "UNMOUNTED",
               audiorcv_usb_streaming() ? "STREAMING" : "IDLE", audiorcv_sample_rate());
    tui_printf(" Audio ring       : %" PRIu32 " / %" PRIu32 " frames  target=%" PRIu32 "\r\n",
               audiorcv_ring_count(), audiorcv_ring_capacity(), a.prebuffer_target);
    tui_printf(" Ring errors      : underflow=%" PRIu32 "  overflow=%" PRIu32 "\r\n\r\n",
               audiorcv_ring_underflows(), audiorcv_ring_overflows());

    tui_printf(" PTP state        : %s  domain=%u\r\n", ptp_state_name(p.state), p.domain);
    tui_printf(" GM Clock ID      : %02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X\r\n",
               p.gm_clock_id[0], p.gm_clock_id[1], p.gm_clock_id[2], p.gm_clock_id[3],
               p.gm_clock_id[4], p.gm_clock_id[5], p.gm_clock_id[6], p.gm_clock_id[7]);
    tui_printf(" GM priority      : %u / %u  class=%u accuracy=0x%02X variance=%u\r\n",
               p.priority1, p.priority2, p.clock_class, p.clock_accuracy, p.clock_variance);
    tui_printf(" Announce / Sync  : age=%" PRIu64 " / %" PRIu64 " ms\r\n",
               p.last_announce_us ? (now_us - p.last_announce_us) / 1000 : 0,
               p.last_sync_us ? (now_us - p.last_sync_us) / 1000 : 0);
    tui_signed_us("Phase current", p.phase_error_ns);
    tui_signed_us("Phase minimum", p.phase_min_ns);
    tui_signed_us("Phase maximum", p.phase_max_ns);
    tui_signed_us("Phase average", p.phase_average_ns);
    tui_signed_us("Mean path delay", p.mean_path_delay_ns);
    tui_printf(" Phase samples    : %" PRIu32 "\r\n", p.measurements);
    tui_printf(" PTP time         : %" PRIu64 ".%09" PRIu64 "\r\n\r\n",
               ptp_ns / 1000000000ull, ptp_ns % 1000000000ull);

    tui_printf(" AES67            : %s  PT=96 L24 stereo\r\n", aes67_state_name(a.state));
    tui_printf(" Destination      : %u.%u.%u.%u:%u  48 frames / 1 ms\r\n",
               a.destination[0], a.destination[1], a.destination[2], a.destination[3], a.port);
    tui_printf(" RTP seq / ts     : %u / %" PRIu32 "  SSRC=%08" PRIX32 "\r\n",
               a.sequence, a.rtp_timestamp, a.ssrc);
    tui_printf(" RTP counters     : sent=%" PRIu32 " err=%" PRIu32 " underrun=%" PRIu32 " late=%" PRIu32 "\r\n\r\n",
               a.packets_sent, a.send_errors, a.underruns, a.late_packets);
    tui_printf(" SAP advertisement: %u.%u.%u.%u:%u  sent=%" PRIu32 " err=%" PRIu32 " hash=%04X\r\n\r\n",
               sap.destination[0], sap.destination[1], sap.destination[2], sap.destination[3], sap.port,
               sap.announcements_sent, sap.send_errors, sap.message_id_hash);
    tui_printf(" PTP packets      : announce=%" PRIu32 " sync=%" PRIu32 " followUp=%" PRIu32
               " delayReq=%" PRIu32 " delayResp=%" PRIu32 "\r\n",
               p.announce_rx, p.sync_rx, p.follow_up_rx, p.delay_req_tx, p.delay_resp_rx);
    tui_printf(" PTP rejected     : malformed=%" PRIu32 " domain=%" PRIu32 " sequence=%" PRIu32 "\r\n",
               p.malformed_rx, p.domain_mismatch, p.sequence_mismatch);
    tui_write("\x1b[J");
}

static void set_config_value(const char *arguments)
{
    char key[20];
    char value[64];
    if (sscanf(arguments, "%19s %63[^\r\n]", key, value) != 2)
    {
        terminal_printf("Usage: config set <ip|mask|gateway|dns|multicast|port|domain|name> <value>\r\n");
        return;
    }
    app_config_t *cfg = config_edit();
    bool valid = true;
    if (strcmp(key, "ip") == 0) valid = parse_ip(value, cfg->ip);
    else if (strcmp(key, "mask") == 0) valid = parse_ip(value, cfg->mask);
    else if (strcmp(key, "gateway") == 0) valid = parse_ip(value, cfg->gateway);
    else if (strcmp(key, "dns") == 0) valid = parse_ip(value, cfg->dns);
    else if (strcmp(key, "multicast") == 0)
    {
        if (strcmp(value, "auto") == 0) cfg->multicast_auto = true;
        else
        {
            valid = parse_ip(value, cfg->multicast) && cfg->multicast[0] >= 224u && cfg->multicast[0] <= 239u;
            if (valid) cfg->multicast_auto = false;
        }
    }
    else if (strcmp(key, "port") == 0)
    {
        uint32_t port;
        valid = parse_u32(value, 65535u, &port) && port > 0u;
        if (valid) cfg->rtp_port = (uint16_t)port;
    }
    else if (strcmp(key, "domain") == 0)
    {
        uint32_t domain;
        valid = parse_u32(value, 127u, &domain);
        if (valid) cfg->ptp_domain = (uint8_t)domain;
    }
    else if (strcmp(key, "name") == 0)
    {
        valid = strlen(value) < CONFIG_STREAM_NAME_LENGTH;
        if (valid) strcpy(cfg->stream_name, value);
    }
    else valid = false;

    if (!valid) { terminal_printf("Invalid config key or value.\r\n"); return; }
    config_mark_dirty();
    terminal_printf("Pending configuration updated. Use 'config apply' or 'config save'.\r\n");
}

static void command_handler(const char *command)
{
    if (strcmp(command, "help") == 0)
    {
        terminal_printf("Commands:\r\n  help | status | tui | ptp | ptp reset-stats | aes67 | sap\r\n"
                        "  config show | config show defaults\r\n"
                        "  config set <ip|mask|gateway|dns|multicast|port|domain|name> <value>\r\n"
                        "  config apply | config save | config defaults\r\n");
    }
    else if (strcmp(command, "status") == 0) { show_ptp(); show_aes67(); show_sap(); }
    else if (strcmp(command, "ptp") == 0) show_ptp();
    else if (strcmp(command, "ptp reset-stats") == 0)
    {
        ptp_reset_phase_statistics(); terminal_printf("PTP phase statistics reset.\r\n");
    }
    else if (strcmp(command, "aes67") == 0) show_aes67();
    else if (strcmp(command, "sap") == 0) show_sap();
    else if (strcmp(command, "config show") == 0)
    {
        show_config_value(config_get(), "Pending configuration");
        terminal_printf("Flash=%s dirty=%s applied-differs=%s\r\n",
                        config_loaded_from_flash() ? "LOADED" : "DEFAULTS",
                        config_is_dirty() ? "YES" : "NO", config_applied_differs() ? "YES" : "NO");
    }
    else if (strcmp(command, "config show defaults") == 0)
    {
        app_config_t defaults; config_set_defaults(&defaults); show_config_value(&defaults, "Defaults");
    }
    else if (strncmp(command, "config set ", 11) == 0) set_config_value(command + 11);
    else if (strcmp(command, "config apply") == 0)
    {
        network_apply_config(config_get()); config_mark_applied(); terminal_printf("Configuration applied (not saved).\r\n");
    }
    else if (strcmp(command, "config save") == 0)
    {
        if (audiorcv_usb_streaming()) terminal_printf("Refused: stop USB Audio before Flash save.\r\n");
        else
        {
            network_apply_config(config_get()); config_mark_applied();
            terminal_printf(config_save() ? "Configuration applied and saved.\r\n" : "Configuration save failed.\r\n");
        }
    }
    else if (strcmp(command, "config defaults") == 0)
    {
        config_set_defaults(config_edit()); config_mark_dirty(); terminal_printf("Defaults loaded as pending configuration.\r\n");
    }
    else if (strcmp(command, "tui") == 0)
    {
        cli_disable(); tui_begin(); tui_set_input_handler(tui_input); tui_active = true; last_tui_draw_us = 0;
    }
    else terminal_printf("Unknown command: %s\r\n", command);
}

int main(void)
{
    stdio_init_all();
    config_init();
    if (!audiorcv_init()) return 1;

    absolute_time_t usb_wait_until = make_timeout_time_ms(3000);
    while (!time_reached(usb_wait_until)) { audiorcv_task(); tight_loop_contents(); }

    cli_init(); cli_set_command_handler(command_handler); cli_enable();
    const app_config_t *cfg = config_get();
    static wiz_NetInfo info = { .mac = {0x02, 0x12, 0x34, 0x56, 0x78, 0x90}, .dhcp = NETINFO_STATIC };
    memcpy(info.ip, cfg->ip, 4); memcpy(info.sn, cfg->mask, 4);
    memcpy(info.gw, cfg->gateway, 4); memcpy(info.dns, cfg->dns, 4);
    if (!network_init(&info)) return 1;
    ptp_init(info.mac, cfg->ptp_domain);
    aes67_init(info.mac);
    sap_init(info.mac);

    while (true)
    {
        audiorcv_task();
        network_task();
        if (tui_active && time_us_64() - last_tui_draw_us >= 250000ull)
        {
            last_tui_draw_us = time_us_64(); draw_tui();
        }
        tight_loop_contents();
    }
}

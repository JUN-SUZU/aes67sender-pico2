#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "wizchip_conf.h"
#include "socket.h"
#include "w5500.h"
#include "network.h"
#include "ptp.h"
#include "aes67.h"
#include "sap.h"

/* =========================================================
 * Pico 2 <-> W5500
 * ========================================================= */
#define W5500_SPI spi0
#define PIN_MISO 16
#define PIN_CS 17
#define PIN_SCK 18
#define PIN_MOSI 19

/* =========================================================
 * Network
 * ========================================================= */
#define TCP_SOCKET 0
#define TCP_PORT 5000

// static wiz_NetInfo network_info = {
//     .mac = {
//         0x02, 0x12, 0x34,
//         0x56, 0x78, 0x90},
//     .ip = {192, 168, 2, 5},
//     .sn = {255, 255, 254, 0},
//     .gw = {192, 168, 2, 1},
//     .dns = {192, 168, 2, 1},
//     .dhcp = NETINFO_STATIC};

/* =========================================================
 * Chip Select
 * ========================================================= */
static void w5500_select(void)
{
    gpio_put(PIN_CS, 0);
}

static void w5500_deselect(void)
{
    gpio_put(PIN_CS, 1);
}

/* =========================================================
 * SPI callback
 * ========================================================= */
static uint8_t w5500_spi_read_byte(void)
{
    uint8_t value = 0;
    spi_read_blocking(
        W5500_SPI,
        0x00,
        &value,
        1);
    return value;
}

static void w5500_spi_write_byte(uint8_t value)
{
    spi_write_blocking(
        W5500_SPI,
        &value,
        1);
}

static void w5500_spi_read_burst(
    uint8_t *buffer,
    uint16_t length)
{
    spi_read_blocking(
        W5500_SPI,
        0x00,
        buffer,
        length);
}

static void w5500_spi_write_burst(
    uint8_t *buffer,
    uint16_t length)
{
    spi_write_blocking(
        W5500_SPI,
        buffer,
        length);
}

/* =========================================================
 * Pico SPI initialization
 * ========================================================= */
static void spi_setup(void)
{
    printf("Initializing SPI0...\n");
    /* PTP software timestamp後のSPI読出し時間を抑える。 */
    spi_init(
        W5500_SPI,
        20 * 1000 * 1000);
    spi_set_format(
        W5500_SPI,
        8,
        SPI_CPOL_0,
        SPI_CPHA_0,
        SPI_MSB_FIRST);
    gpio_set_function(
        PIN_MISO,
        GPIO_FUNC_SPI);
    gpio_set_function(
        PIN_SCK,
        GPIO_FUNC_SPI);
    gpio_set_function(
        PIN_MOSI,
        GPIO_FUNC_SPI);
    /*
     * CSはSPIペリフェラルには任せず、
     * ioLibraryからGPIOとして制御する。
     */
    gpio_init(PIN_CS);
    gpio_set_dir(PIN_CS, GPIO_OUT);
    gpio_put(PIN_CS, 1);
    sleep_ms(100);
}

/* =========================================================
 * W5500 initialization
 * ========================================================= */
static bool w5500_setup(wiz_NetInfo *info)
{
    uint8_t tx_size[8] = {
        2, 2, 2, 2,
        2, 2, 2, 2};
    uint8_t rx_size[8] = {
        2, 2, 2, 2,
        2, 2, 2, 2};

    /*
     * ioLibraryにPico側の処理を登録
     */
    reg_wizchip_cs_cbfunc(
        w5500_select,
        w5500_deselect);
    reg_wizchip_spi_cbfunc(
        w5500_spi_read_byte,
        w5500_spi_write_byte);
    reg_wizchip_spiburst_cbfunc(
        w5500_spi_read_burst,
        w5500_spi_write_burst);

    /*
     * W5500内部ソケット用メモリ
     *
     * 8 sockets × 2 KiB
     */
    if (wizchip_init(tx_size, rx_size) != 0)
    {
        printf("ERROR: wizchip_init failed\n");
        return false;
    }

    /*
     * VERSIONR確認
     */
    uint8_t version = getVERSIONR();
    printf(
        "W5500 VERSIONR = 0x%02X\n",
        version);

    /*
     * W5500なら0x04
     */
    if (version != 0x04)
    {
        printf("\n");
        printf("ERROR: W5500 was not detected.\n");
        printf("Expected VERSIONR: 0x04\n");
        printf("Received VERSIONR: 0x%02X\n", version);
        printf("\n");
        printf("Check:\n");
        printf("  MISO -> GP16\n");
        printf("  CS   -> GP17\n");
        printf("  SCLK -> GP18\n");
        printf("  MOSI -> GP19\n");
        printf("  GND  -> GND\n");
        printf("  3.3V -> 3V3\n");
        return false;
    }

    printf("W5500 detected successfully.\n");

    /*
     * IPアドレス等を設定
     */
    wizchip_setnetinfo(
        info);

    return true;
}

/* =========================================================
 * Network information
 * ========================================================= */
static void print_network_info(void)
{
    wiz_NetInfo info = {0};
    wizchip_getnetinfo(
        &info);

    printf("\n");
    printf("========== Network ==========\n");
    printf(
        "MAC  : %02X:%02X:%02X:%02X:%02X:%02X\n",
        info.mac[0],
        info.mac[1],
        info.mac[2],
        info.mac[3],
        info.mac[4],
        info.mac[5]);

    printf(
        "IP   : %u.%u.%u.%u\n",
        info.ip[0],
        info.ip[1],
        info.ip[2],
        info.ip[3]);

    printf(
        "MASK : %u.%u.%u.%u\n",
        info.sn[0],
        info.sn[1],
        info.sn[2],
        info.sn[3]);

    printf(
        "GW   : %u.%u.%u.%u\n",
        info.gw[0],
        info.gw[1],
        info.gw[2],
        info.gw[3]);

    printf("=============================\n");
}

/* =========================================================
 * PHY Link
 * ========================================================= */
bool network_link_is_up(void)
{
    uint8_t link = PHY_LINK_OFF;
    ctlwizchip(
        CW_GET_PHYLINK,
        &link);
    return link == PHY_LINK_ON;
}

void network_get_info(wiz_NetInfo *info)
{
    if (info != NULL) wizchip_getnetinfo(info);
}

void network_apply_config(const app_config_t *config)
{
    if (config == NULL) return;
    for (uint8_t sn = 0; sn < 4; ++sn)
    {
        if (getSn_SR(sn) != SOCK_CLOSED) close(sn);
    }
    wiz_NetInfo info = {
        .mac = {0x02, 0x12, 0x34, 0x56, 0x78, 0x90},
        .dhcp = NETINFO_STATIC
    };
    wiz_NetInfo old_info;
    wizchip_getnetinfo(&old_info);
    memcpy(info.mac, old_info.mac, sizeof(info.mac));
    memcpy(info.ip, config->ip, 4);
    memcpy(info.sn, config->mask, 4);
    memcpy(info.gw, config->gateway, 4);
    memcpy(info.dns, config->dns, 4);
    wizchip_setnetinfo(&info);
    ptp_set_domain(config->ptp_domain);
    aes67_reconfigure();
    sap_reconfigure();
}

/* =========================================================
 * TCP Echo Server
 * ========================================================= */
static void tcp_echo_server(void)
{
    static uint8_t buffer[2048];
    uint8_t status = getSn_SR(
        TCP_SOCKET);

    switch (status)
    {
    /*
     * Socket closed
     */
    case SOCK_CLOSED:
    {
        int8_t result;
        result = socket(
            TCP_SOCKET,
            Sn_MR_TCP,
            TCP_PORT,
            0);
        if (result != TCP_SOCKET)
        {
            printf(
                "socket() failed: %d\n",
                result);
            sleep_ms(1000);
            return;
        }

        printf(
            "TCP socket opened. port=%u\n",
            TCP_PORT);
        break;
    }

    /*
     * Socket initialized
     */
    case SOCK_INIT:
    {
        int8_t result;
        result = listen(
            TCP_SOCKET);
        if (result != SOCK_OK)
        {
            printf(
                "listen() failed: %d\n",
                result);
            close(
                TCP_SOCKET);
            return;
        }

        printf(
            "Listening on 192.168.2.5:%u\n",
            TCP_PORT);
        break;
    }

    /*
     * Waiting for connection
     */
    case SOCK_LISTEN:
    {
        break;
    }

    /*
     * Client connected
     */
    case SOCK_ESTABLISHED:
    {
        /*
         * 接続成立割り込みフラグを解除
         */
        if (
            getSn_IR(TCP_SOCKET) & Sn_IR_CON)
        {
            printf(
                "Client connected.\n");
            setSn_IR(
                TCP_SOCKET,
                Sn_IR_CON);
        }

        uint16_t received_size;
        received_size =
            getSn_RX_RSR(
                TCP_SOCKET);

        if (received_size == 0)
        {
            break;
        }

        if (
            received_size > sizeof(buffer))
        {
            received_size =
                sizeof(buffer);
        }

        int32_t length;
        length = recv(
            TCP_SOCKET,
            buffer,
            received_size);

        if (length <= 0)
        {
            break;
        }

        printf(
            "Received %ld bytes: ",
            (long)length);

        for (
            int32_t i = 0;
            i < length;
            i++)
        {
            putchar(
                (char)buffer[i]);
        }
        printf("\n");

        /*
         * 受信したデータをそのまま送り返す
         */
        int32_t sent;
        sent = send(
            TCP_SOCKET,
            buffer,
            (uint16_t)length);

        printf(
            "Echoed %ld bytes\n",
            (long)sent);

        break;
    }

    /*
     * 相手が切断開始
     */
    case SOCK_CLOSE_WAIT:
    {
        printf(
            "Client disconnected.\n");
        disconnect(
            TCP_SOCKET);
        close(
            TCP_SOCKET);
        break;
    }

    default:
    {
        break;
    }
    }
}

/* =========================================================
 * Module entry points
 * ========================================================= */
bool network_init(wiz_NetInfo *info)
{
    printf("\n");
    printf("==============================\n");
    printf(" Pico 2 + W5500 Network Test\n");
    printf("==============================\n");

    spi_setup();

    if (!w5500_setup(info))
    {
        printf("W5500 initialization failed.\n");
        return false;
    }

    print_network_info();
    printf("Waiting for Ethernet link...\n");
    return true;
}

void network_task(void)
{
    static bool previous_link = false;

    bool link = network_link_is_up();
    if (link != previous_link)
    {
        printf("Ethernet LINK: %s\n", link ? "UP" : "DOWN");
        previous_link = link;
    }

    if (!link)
    {
        ptp_task(false);
        aes67_task(false);
        sap_task(false);
        return;
    }

    tcp_echo_server();
    ptp_task(true);
    aes67_task(true);
    sap_task(true);
}

#include "terminal.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "pico/stdio/driver.h"
#include "tusb.h"

#define TERMINAL_STARTUP_CACHE_SIZE 100u
#define TERMINAL_PRINTF_BUFFER_SIZE 256u
#define TERMINAL_TX_BUFFER_SIZE 4096u

static char startup_cache[TERMINAL_STARTUP_CACHE_SIZE];

static size_t startup_cache_length = 0;

static bool usb_ready = false;
static bool was_connected = false;

static char tx_buffer[TERMINAL_TX_BUFFER_SIZE];
static size_t tx_read_index;
static size_t tx_write_index;

static size_t tx_count(void)
{
    return tx_write_index - tx_read_index;
}

static void tx_append(const char *data, size_t length)
{
    while (length > 0 && tx_count() < TERMINAL_TX_BUFFER_SIZE)
    {
        tx_buffer[tx_write_index % TERMINAL_TX_BUFFER_SIZE] = *data++;
        tx_write_index++;
        length--;
    }
}

static terminal_input_handler_t input_handler = NULL;
static void *input_handler_user_data = NULL;

//--------------------------------------------------------------------+
// Startup cache
//--------------------------------------------------------------------+

static void terminal_cache_append(
    const char *data,
    size_t length)
{
    if (length == 0)
    {
        return;
    }

    /*
     * 100byte以上なら末尾100byteのみ保持
     */
    if (length >= TERMINAL_STARTUP_CACHE_SIZE)
    {
        memcpy(
            startup_cache,
            data + length - TERMINAL_STARTUP_CACHE_SIZE,
            TERMINAL_STARTUP_CACHE_SIZE);

        startup_cache_length =
            TERMINAL_STARTUP_CACHE_SIZE;

        return;
    }

    /*
     * 入らない場合は古い内容から削除
     */
    if (
        startup_cache_length + length >
        TERMINAL_STARTUP_CACHE_SIZE)
    {
        size_t remove =
            startup_cache_length +
            length -
            TERMINAL_STARTUP_CACHE_SIZE;

        memmove(
            startup_cache,
            startup_cache + remove,
            startup_cache_length - remove);

        startup_cache_length -= remove;
    }

    memcpy(
        startup_cache + startup_cache_length,
        data,
        length);

    startup_cache_length += length;
}

//--------------------------------------------------------------------+
// Low-level output
//--------------------------------------------------------------------+

static void terminal_output(
    const char *data,
    size_t length)
{
    if (length == 0)
    {
        return;
    }

    /*
     * TinyUSB初期化前、またはCOM未接続
     */
    if (
        !usb_ready ||
        !tud_cdc_connected())
    {
        terminal_cache_append(
            data,
            length);

        return;
    }

    tx_append(data, length);
}

//--------------------------------------------------------------------+
// Pico stdio
//--------------------------------------------------------------------+

static void terminal_stdio_out_chars(
    const char *buf,
    int len
)
{
    if (buf == NULL || len <= 0)
    {
        return;
    }

    static bool previous_was_cr = false;

    for (int i = 0; i < len; i++)
    {
        char ch = buf[i];

        if (ch == '\n')
        {
            if (!previous_was_cr)
            {
                terminal_output(
                    "\r",
                    1
                );
            }

            terminal_output(
                "\n",
                1
            );

            previous_was_cr = false;
        }
        else
        {
            terminal_output(
                &ch,
                1
            );

            previous_was_cr =
                (ch == '\r');
        }
    }
}

static stdio_driver_t terminal_stdio_driver =
    {
        .out_chars = terminal_stdio_out_chars};

//--------------------------------------------------------------------+
// Public API
//--------------------------------------------------------------------+

void terminal_init(void)
{
    startup_cache_length = 0;
    tx_read_index = 0;
    tx_write_index = 0;

    usb_ready = false;
    was_connected = false;

    input_handler = NULL;
    input_handler_user_data = NULL;

    stdio_set_driver_enabled(
        &terminal_stdio_driver,
        true);
}

void terminal_set_usb_ready(bool ready)
{
    usb_ready = ready;
}

bool terminal_connected(void)
{
    return usb_ready &&
           tud_cdc_connected();
}

void terminal_write(
    const void *data,
    size_t length)
{
    terminal_output(
        (const char *)data,
        length);
}

void terminal_write_string(
    const char *str)
{
    if (str == NULL)
    {
        return;
    }

    terminal_output(
        str,
        strlen(str));
}

void terminal_printf(
    const char *format,
    ...)
{
    char buffer[TERMINAL_PRINTF_BUFFER_SIZE];

    va_list args;

    va_start(args, format);

    int length =
        vsnprintf(
            buffer,
            sizeof(buffer),
            format,
            args);

    va_end(args);

    if (length <= 0)
    {
        return;
    }

    size_t output_length =
        (size_t)length;

    if (output_length >= sizeof(buffer))
    {
        output_length =
            sizeof(buffer) - 1;
    }

    terminal_output(
        buffer,
        output_length);
}

void terminal_set_input_handler(
    terminal_input_handler_t handler,
    void *user_data)
{
    input_handler = handler;
    input_handler_user_data = user_data;
}

void terminal_task(void)
{
    if (!usb_ready)
    {
        return;
    }

    bool connected =
        tud_cdc_connected();

    /*
     * COMポートが開かれた瞬間
     */
    if (
        connected &&
        !was_connected)
    {
        /*
         * 起動時ログを吐く
         */
        if (startup_cache_length > 0)
        {
            tx_append(startup_cache, startup_cache_length);

            startup_cache_length = 0;
        }
    }

    was_connected = connected;

    if (!connected)
    {
        return;
    }

    while (tx_count() > 0)
    {
        uint32_t available = tud_cdc_write_available();
        if (available == 0) break;
        size_t contiguous = TERMINAL_TX_BUFFER_SIZE - (tx_read_index % TERMINAL_TX_BUFFER_SIZE);
        if (contiguous > tx_count()) contiguous = tx_count();
        if (contiguous > available) contiguous = available;
        uint32_t written = tud_cdc_write(
            tx_buffer + (tx_read_index % TERMINAL_TX_BUFFER_SIZE), (uint32_t)contiguous);
        if (written == 0) break;
        tx_read_index += written;
        if (tx_read_index == tx_write_index)
        {
            tx_read_index = 0;
            tx_write_index = 0;
        }
    }

    /*
     * RX
     */
    while (tud_cdc_available())
    {
        uint8_t buffer[64];

        uint32_t count =
            tud_cdc_read(
                buffer,
                sizeof(buffer));

        if (input_handler != NULL)
        {
            for (
                uint32_t i = 0;
                i < count;
                i++)
            {
                input_handler(
                    buffer[i],
                    input_handler_user_data);
            }
        }
    }

    /*
     * printf/TUI/CLIすべてまとめて送信
     */
    tud_cdc_write_flush();
}

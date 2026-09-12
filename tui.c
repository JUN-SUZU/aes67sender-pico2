#include "tui.h"

#include <stdio.h>
#include <stdarg.h>

#include "terminal.h"

#define TUI_PRINTF_BUFFER_SIZE 256u

static tui_input_handler_t tui_input_handler =
    NULL;

//--------------------------------------------------------------------+

static void tui_terminal_input(
    uint8_t ch,
    void *user_data)
{
    (void)user_data;

    if (tui_input_handler != NULL)
    {
        tui_input_handler(ch);
    }
}

//--------------------------------------------------------------------+

void tui_begin(void)
{
    /*
     * Alternate Screen Buffer
     */
    terminal_write_string(
        "\x1b[?1049h");

    tui_hide_cursor();

    tui_clear();

    terminal_set_input_handler(
        tui_terminal_input,
        NULL);
}

void tui_end(void)
{
    tui_show_cursor();

    /*
     * Alternate Screen終了
     */
    terminal_write_string(
        "\x1b[?1049l");

    terminal_set_input_handler(
        NULL,
        NULL);
}

//--------------------------------------------------------------------+

void tui_set_input_handler(
    tui_input_handler_t handler)
{
    tui_input_handler =
        handler;
}

//--------------------------------------------------------------------+

void tui_clear(void)
{
    terminal_write_string(
        "\x1b[2J");

    tui_home();
}

void tui_home(void)
{
    terminal_write_string(
        "\x1b[H");
}

void tui_move(
    uint16_t row,
    uint16_t column)
{
    terminal_printf(
        "\x1b[%u;%uH",
        row,
        column);
}

void tui_clear_line(void)
{
    terminal_write_string(
        "\x1b[2K");
}

void tui_hide_cursor(void)
{
    terminal_write_string(
        "\x1b[?25l");
}

void tui_show_cursor(void)
{
    terminal_write_string(
        "\x1b[?25h");
}

//--------------------------------------------------------------------+

void tui_write(
    const char *text)
{
    terminal_write_string(text);
}

void tui_printf(
    const char *format,
    ...)
{
    char buffer[TUI_PRINTF_BUFFER_SIZE];

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

    if (
        output_length >=
        sizeof(buffer))
    {
        output_length =
            sizeof(buffer) - 1;
    }

    terminal_write(
        buffer,
        output_length);
}

//--------------------------------------------------------------------+

void tui_set_bold(bool enabled)
{
    terminal_write_string(
        enabled
            ? "\x1b[1m"
            : "\x1b[22m");
}

void tui_set_reverse(bool enabled)
{
    terminal_write_string(
        enabled
            ? "\x1b[7m"
            : "\x1b[27m");
}

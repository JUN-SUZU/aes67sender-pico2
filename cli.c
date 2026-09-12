#include "cli.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "terminal.h"

#define CLI_COMMAND_LENGTH 100u
#define CLI_HISTORY_COUNT 8u

static char command_buffer[CLI_COMMAND_LENGTH];

static size_t command_length = 0;

static char history[CLI_HISTORY_COUNT][CLI_COMMAND_LENGTH];

static size_t history_count = 0;
static size_t history_position = 0;

static char history_scratch[CLI_COMMAND_LENGTH];

static size_t history_scratch_length = 0;

static cli_command_handler_t command_handler =
    NULL;

typedef enum
{
    ESCAPE_NONE,
    ESCAPE_ESC,
    ESCAPE_CSI

} escape_state_t;

static escape_state_t escape_state =
    ESCAPE_NONE;

static bool ignore_next_lf = false;

//--------------------------------------------------------------------+

static void cli_redraw(void)
{
    terminal_write_string(
        "\r\x1b[2K> ");

    terminal_write(
        command_buffer,
        command_length);
}

//--------------------------------------------------------------------+

static void history_add(
    const char *command)
{
    if (*command == '\0')
    {
        return;
    }

    if (
        history_count > 0 &&
        strcmp(
            history[history_count - 1],
            command) == 0)
    {
        history_position =
            history_count;

        return;
    }

    if (
        history_count <
        CLI_HISTORY_COUNT)
    {
        strncpy(
            history[history_count],
            command,
            CLI_COMMAND_LENGTH - 1);

        history[history_count][CLI_COMMAND_LENGTH - 1] = '\0';

        history_count++;
    }
    else
    {
        memmove(
            history[0],
            history[1],
            sizeof(history[0]) *
                (CLI_HISTORY_COUNT - 1));

        strncpy(
            history[CLI_HISTORY_COUNT - 1],
            command,
            CLI_COMMAND_LENGTH - 1);

        history[CLI_HISTORY_COUNT - 1][CLI_COMMAND_LENGTH - 1] = '\0';
    }

    history_position =
        history_count;
}

//--------------------------------------------------------------------+

static void history_up(void)
{
    if (history_count == 0)
    {
        return;
    }

    if (
        history_position ==
        history_count)
    {
        memcpy(
            history_scratch,
            command_buffer,
            command_length);

        history_scratch[command_length] = '\0';

        history_scratch_length =
            command_length;
    }

    if (history_position > 0)
    {
        history_position--;
    }

    strncpy(
        command_buffer,
        history[history_position],
        CLI_COMMAND_LENGTH - 1);

    command_buffer[CLI_COMMAND_LENGTH - 1] = '\0';

    command_length =
        strlen(command_buffer);

    cli_redraw();
}

//--------------------------------------------------------------------+

static void history_down(void)
{
    if (
        history_position >=
        history_count)
    {
        return;
    }

    history_position++;

    if (
        history_position ==
        history_count)
    {
        memcpy(
            command_buffer,
            history_scratch,
            history_scratch_length);

        command_length =
            history_scratch_length;

        command_buffer[command_length] = '\0';
    }
    else
    {
        strncpy(
            command_buffer,
            history[history_position],
            CLI_COMMAND_LENGTH - 1);

        command_buffer[CLI_COMMAND_LENGTH - 1] = '\0';

        command_length =
            strlen(command_buffer);
    }

    cli_redraw();
}

//--------------------------------------------------------------------+

static void execute_command(void)
{
    terminal_write_string("\r\n");

    command_buffer[command_length] = '\0';

    if (command_length > 0)
    {
        history_add(
            command_buffer);

        if (command_handler != NULL)
        {
            command_handler(
                command_buffer);
        }
        else
        {
            terminal_printf(
                "No command handler: %s\r\n",
                command_buffer);
        }
    }

    command_length = 0;

    command_buffer[0] = '\0';

    history_position =
        history_count;

    history_scratch_length = 0;

    cli_show_prompt();
}

//--------------------------------------------------------------------+

static void cli_input(
    uint8_t ch,
    void *user_data)
{
    (void)user_data;

    /*
     * ANSI escape
     */
    if (escape_state == ESCAPE_ESC)
    {
        if (ch == '[')
        {
            escape_state =
                ESCAPE_CSI;
        }
        else
        {
            escape_state =
                ESCAPE_NONE;
        }

        return;
    }

    if (escape_state == ESCAPE_CSI)
    {
        if (ch == 'A')
        {
            history_up();
        }
        else if (ch == 'B')
        {
            history_down();
        }

        escape_state =
            ESCAPE_NONE;

        return;
    }

    if (ch == 0x1B)
    {
        escape_state =
            ESCAPE_ESC;

        return;
    }

    /*
     * Enter
     */
    if (ch == '\r')
    {
        ignore_next_lf = true;

        execute_command();

        return;
    }

    if (ch == '\n')
    {
        if (ignore_next_lf)
        {
            ignore_next_lf = false;
            return;
        }

        execute_command();

        return;
    }

    ignore_next_lf = false;

    /*
     * Backspace / DEL
     */
    if (
        ch == 0x08 ||
        ch == 0x7F)
    {
        if (command_length > 0)
        {
            command_length--;

            command_buffer[command_length] = '\0';

            terminal_write_string(
                "\b \b");
        }

        return;
    }

    if (ch < 0x20)
    {
        return;
    }

    if (
        command_length >=
        CLI_COMMAND_LENGTH - 1)
    {
        return;
    }

    command_buffer[command_length++] = (char)ch;

    command_buffer[command_length] = '\0';

    terminal_write(
        &ch,
        1);
}

//--------------------------------------------------------------------+

void cli_init(void)
{
    command_length = 0;
    command_buffer[0] = '\0';

    history_count = 0;
    history_position = 0;

    escape_state =
        ESCAPE_NONE;

    command_handler = NULL;
}

void cli_enable(void)
{
    terminal_set_input_handler(
        cli_input,
        NULL);

    cli_show_prompt();
}

void cli_disable(void)
{
    terminal_set_input_handler(
        NULL,
        NULL);
}

void cli_set_command_handler(
    cli_command_handler_t handler)
{
    command_handler = handler;
}

void cli_show_prompt(void)
{
    terminal_write_string("> ");
}

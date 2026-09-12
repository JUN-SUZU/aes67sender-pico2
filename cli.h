#pragma once

typedef void (*cli_command_handler_t)(
    const char *command);

void cli_init(void);

void cli_enable(void);

void cli_disable(void);

void cli_set_command_handler(
    cli_command_handler_t handler);

void cli_show_prompt(void);

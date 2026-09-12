#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef void (*tui_input_handler_t)(
    uint8_t key);

void tui_begin(void);

void tui_end(void);

void tui_set_input_handler(
    tui_input_handler_t handler);

void tui_clear(void);

void tui_home(void);

void tui_move(
    uint16_t row,
    uint16_t column);

void tui_clear_line(void);

void tui_hide_cursor(void);

void tui_show_cursor(void);

void tui_write(
    const char *text);

void tui_printf(
    const char *format,
    ...);

void tui_set_bold(bool enabled);

void tui_set_reverse(bool enabled);

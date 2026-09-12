#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef void (*terminal_input_handler_t)(
    uint8_t ch,
    void *user_data);

void terminal_init(void);

/*
 * tusb_init()完了後にtrueを設定する。
 */
void terminal_set_usb_ready(bool ready);

/*
 * audiorcv_task()から高頻度で呼ぶ。
 */
void terminal_task(void);

/*
 * 接続状態
 */
bool terminal_connected(void);

/*
 * 生データ出力
 */
void terminal_write(
    const void *data,
    size_t length);

void terminal_write_string(
    const char *str);

/*
 * printf形式。
 *
 * 通常のprintf()もstdio driver経由で同じCDCへ流れる。
 */
void terminal_printf(
    const char *format,
    ...);

/*
 * CDCから受信した1byteごとの通知先。
 *
 * CLI / TUIで切り替えて使用可能。
 */
void terminal_set_input_handler(
    terminal_input_handler_t handler,
    void *user_data);

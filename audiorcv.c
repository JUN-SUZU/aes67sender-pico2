/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2020 Jerzy Kasenberg
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include <stdio.h>
#include <string.h>
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/irq.h"
#include "hardware/clocks.h"
#include "hardware/sync.h"
// #include "audio_output.h"
#include "pico/stdlib.h"
#include "pico/stdio/driver.h"
#include "common_types.h"
#include "tusb.h"
#include "usb_descriptors.h"
#include "audiorcv.h"
#include "terminal.h"

//--------------------------------------------------------------------+
// MACRO CONSTANT TYPEDEF PROTOTYPES
//--------------------------------------------------------------------+

/* Blink pattern
 * - 25 ms   : streaming data
 * - 250 ms  : device not mounted
 * - 1000 ms : device mounted
 * - 2500 ms : device is suspended
 */
enum {
  BLINK_STREAMING = 25,
  BLINK_NOT_MOUNTED = 250,
  BLINK_MOUNTED = 1000,
  BLINK_SUSPENDED = 2500,
};

enum {
  VOLUME_CTRL_0_DB = 0,
  VOLUME_CTRL_10_DB = 2560,
  VOLUME_CTRL_20_DB = 5120,
  VOLUME_CTRL_30_DB = 7680,
  VOLUME_CTRL_40_DB = 10240,
  VOLUME_CTRL_50_DB = 12800,
  VOLUME_CTRL_60_DB = 15360,
  VOLUME_CTRL_70_DB = 17920,
  VOLUME_CTRL_80_DB = 20480,
  VOLUME_CTRL_90_DB = 23040,
  VOLUME_CTRL_100_DB = 25600,
  VOLUME_CTRL_SILENCE = 0x8000,
};

static uint32_t blink_interval_ms = BLINK_NOT_MOUNTED;

// Audio controls
// Current states
uint8_t mute[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1];   // +1 for master channel 0
int16_t volume[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1];// +1 for master channel 0
uint32_t current_sample_rate = 48000;

#define AUDIO_PWM_GPIO      20u
#define AUDIO_PWM_TOP       255u

#define AUDIO_RING_FRAMES   2048u
#define AUDIO_RING_MASK     (AUDIO_RING_FRAMES - 1u)

static audio_stereo_frame_t audio_ring[AUDIO_RING_FRAMES];

static volatile uint32_t audio_write_index = 0;
static volatile uint32_t audio_read_index  = 0;
static volatile bool network_consumer_enabled = false;
static volatile uint32_t audio_ring_overflow_count = 0;
static volatile uint32_t audio_ring_underflow_count = 0;
static volatile bool usb_audio_streaming = false;

static uint audio_pwm_slice;
static uint audio_pwm_channel;

static uint audio_pacer_slice;

// Buffer for speaker data
uint16_t i2s_dummy_buffer[CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ / 2];

void led_blinking_task(void);
void audio_task(void);
void cdc_task(void);
static void cdc_cache_write(const char *buf, size_t len);

static void __isr audio_sample_irq(void);

static uint32_t audiorcv_millis(void)
{
    return to_ms_since_boot(get_absolute_time());
}

static void audiorcv_led_write(bool state)
{
#ifdef PICO_DEFAULT_LED_PIN
    gpio_put(PICO_DEFAULT_LED_PIN, state);
#else
    (void)state;
#endif
}

static inline uint32_t audio_ring_count(void)
{
    return audio_write_index - audio_read_index;
}

static void __isr audio_sample_irq(void)
{
    pwm_clear_irq(audio_pacer_slice);

    /* AES67送信中はring bufferのconsumerをネットワーク側へ渡す。 */
    if (network_consumer_enabled)
    {
        pwm_set_chan_level(audio_pwm_slice, audio_pwm_channel, 128);
        return;
    }

    if (audio_ring_count() == 0)
    {
        if (usb_audio_streaming)
        {
            audio_ring_underflow_count++;
        }
        pwm_set_chan_level(
            audio_pwm_slice,
            audio_pwm_channel,
            128
        );

        return;
    }

    uint32_t r = audio_read_index;

    audio_stereo_frame_t frame =
        audio_ring[r & AUDIO_RING_MASK];

    audio_read_index = r + 1u;

    /*
     * Stereo -> mono
     */
    int32_t mono =
        ((int32_t)frame.left +
         (int32_t)frame.right) / 2;

    /*
     * signed 16 bit:
     *
     * -32768 ... +32767
     *
     * ↓
     *
     * 0 ... 65535
     *
     * ↓
     *
     * 0 ... 255
     */
    uint16_t unsigned_sample =
        (uint16_t)(mono + 32768);

    uint8_t pwm_value =
        (uint8_t)(unsigned_sample >> 8);

    pwm_set_chan_level(
        audio_pwm_slice,
        audio_pwm_channel,
        pwm_value
    );
}

bool audiorcv_usb_mounted(void)
{
    return tud_mounted();
}

bool audiorcv_usb_streaming(void)
{
    return usb_audio_streaming;
}

uint32_t audiorcv_sample_rate(void)
{
    return current_sample_rate;
}

uint32_t audiorcv_ring_count(void)
{
    return audio_ring_count();
}

uint32_t audiorcv_ring_capacity(void)
{
    return AUDIO_RING_FRAMES;
}

uint32_t audiorcv_ring_overflows(void)
{
    return audio_ring_overflow_count;
}

uint32_t audiorcv_ring_underflows(void)
{
    return audio_ring_underflow_count;
}

void audiorcv_set_network_consumer(bool enabled)
{
    uint32_t irq_state = save_and_disable_interrupts();
    network_consumer_enabled = enabled;
    restore_interrupts(irq_state);
}

size_t audiorcv_read_frames(audio_stereo_frame_t *frames, size_t count)
{
    if (frames == NULL || count == 0)
    {
        return 0;
    }

    uint32_t irq_state = save_and_disable_interrupts();
    uint32_t available = audio_ring_count();
    if (count > available)
    {
        count = available;
    }

    for (size_t i = 0; i < count; ++i)
    {
        frames[i] = audio_ring[(audio_read_index + (uint32_t)i) & AUDIO_RING_MASK];
    }
    audio_read_index += (uint32_t)count;
    restore_interrupts(irq_state);
    return count;
}

static void audio_pwm_init(void);
static void audio_pwm_init(void)
{
    /*
     * --------------------------------------------------------
     * 実際のオーディオPWM
     * --------------------------------------------------------
     */

    gpio_set_function(
        AUDIO_PWM_GPIO,
        GPIO_FUNC_PWM
    );

    audio_pwm_slice =
        pwm_gpio_to_slice_num(AUDIO_PWM_GPIO);

    audio_pwm_channel =
        pwm_gpio_to_channel(AUDIO_PWM_GPIO);

    pwm_config dac_config =
        pwm_get_default_config();

    pwm_config_set_clkdiv(
        &dac_config,
        1.0f
    );

    pwm_config_set_wrap(
        &dac_config,
        AUDIO_PWM_TOP
    );

    pwm_init(
        audio_pwm_slice,
        &dac_config,
        true
    );

    pwm_set_chan_level(
        audio_pwm_slice,
        audio_pwm_channel,
        128
    );


    /*
     * --------------------------------------------------------
     * 48 kHzサンプルクロック用PWM slice
     *
     * GPIOには出さない
     * --------------------------------------------------------
     */

    audio_pacer_slice =
        (audio_pwm_slice + 1u) % NUM_PWM_SLICES;

    uint32_t clk =
        clock_get_hz(clk_sys);

    /*
     * Pico 2標準150 MHzなら
     *
     * 150000000 / 48000
     * = 3125
     *
     * TOP = 3124
     *
     * → 正確に48 kHz
     */
    uint32_t counts =
        clk / 48000u;

    pwm_config pacer_config =
        pwm_get_default_config();

    pwm_config_set_clkdiv(
        &pacer_config,
        1.0f
    );

    pwm_config_set_wrap(
        &pacer_config,
        counts - 1u
    );

    pwm_clear_irq(audio_pacer_slice);

    irq_set_exclusive_handler(
        PWM_DEFAULT_IRQ_NUM(),
        audio_sample_irq
    );

    pwm_set_irq_enabled(
        audio_pacer_slice,
        true
    );

    irq_set_enabled(
        PWM_DEFAULT_IRQ_NUM(),
        true
    );

    pwm_init(
        audio_pacer_slice,
        &pacer_config,
        true
    );
}

//--------------------------------------------------------------------+
// CDC STDIO DRIVER
//--------------------------------------------------------------------+

static void cdc_stdio_out_chars(const char *buf, int len)
{
    if (len <= 0)
    {
        return;
    }

    /*
     * COMポートが開かれていない場合はキャッシュする。
     *
     * ここでは接続待ちはしない。
     */
    if (!tud_cdc_connected())
    {
        cdc_cache_write(buf, (size_t)len);
        return;
    }

    while (len > 0)
    {
        uint32_t available = tud_cdc_write_available();

        if (available == 0)
        {
            /*
             * TX FIFOがいっぱい。
             *
             * printf() 内で待つとUSB処理を止める可能性があるので、
             * 残りは破棄して戻る。
             */
            break;
        }

        uint32_t write_len = (uint32_t)len;

        if (write_len > available)
        {
            write_len = available;
        }

        uint32_t written =
            tud_cdc_write(
                buf,
                write_len
            );

        if (written == 0)
        {
            break;
        }

        buf += written;
        len -= (int)written;
    }
}

// CDC Cache
#define CDC_STARTUP_CACHE_SIZE 240

static uint8_t cdc_startup_cache[CDC_STARTUP_CACHE_SIZE];
static size_t cdc_startup_cache_len = 0;

/*
 * COMポート未接続中の出力を保存する。
 *
 * 容量を超えた場合は古いデータから捨てて、
 * 常に最新 CDC_STARTUP_CACHE_SIZE byte を保持する。
 */
static void cdc_cache_write(const char *buf, size_t len)
{
    if (buf == NULL || len == 0)
    {
        return;
    }

    /*
     * 今回のデータだけでキャッシュ全体を超える場合、
     * 今回のデータの末尾だけ保存する。
     */
    if (len >= CDC_STARTUP_CACHE_SIZE)
    {
        memcpy(
            cdc_startup_cache,
            buf + len - CDC_STARTUP_CACHE_SIZE,
            CDC_STARTUP_CACHE_SIZE
        );

        cdc_startup_cache_len = CDC_STARTUP_CACHE_SIZE;
        return;
    }

    /*
     * 既存データ + 新規データが容量を超える場合、
     * 古いデータを前から削除する。
     */
    size_t required = cdc_startup_cache_len + len;

    if (required > CDC_STARTUP_CACHE_SIZE)
    {
        size_t remove_len = required - CDC_STARTUP_CACHE_SIZE;

        memmove(
            cdc_startup_cache,
            cdc_startup_cache + remove_len,
            cdc_startup_cache_len - remove_len
        );

        cdc_startup_cache_len -= remove_len;
    }

    memcpy(
        cdc_startup_cache + cdc_startup_cache_len,
        buf,
        len
    );

    cdc_startup_cache_len += len;
}

static void cdc_flush_startup_cache(void)
{
    while (cdc_startup_cache_len > 0)
    {
        uint32_t available = tud_cdc_write_available();

        if (available == 0)
        {
            return;
        }

        uint32_t write_len = (uint32_t)cdc_startup_cache_len;

        if (write_len > available)
        {
            write_len = available;
        }

        uint32_t written =
            tud_cdc_write(
                cdc_startup_cache,
                write_len
            );

        if (written == 0)
        {
            return;
        }

        if (written < cdc_startup_cache_len)
        {
            memmove(
                cdc_startup_cache,
                cdc_startup_cache + written,
                cdc_startup_cache_len - written
            );
        }

        cdc_startup_cache_len -= written;
    }

    tud_cdc_write_flush();
}


static stdio_driver_t cdc_stdio_driver =
{
    .out_chars = cdc_stdio_out_chars,
};


#if CFG_AUDIO_DEBUG
void audio_debug_task(void);
uint8_t current_alt_settings;
volatile uint16_t fifo_count;
volatile uint32_t fifo_count_avg;
#endif

/*------------- MODULE ENTRY POINTS -------------*/
bool audiorcv_init(void)
{
#ifdef PICO_DEFAULT_LED_PIN
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
#endif

    terminal_init();

    tusb_rhport_init_t dev_init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_AUTO
    };

    if (!tusb_init(0, &dev_init))
    {
        return false;
    }

    terminal_set_usb_ready(true);

    audio_pwm_init();

    printf("audiorcv initialized\r\n");
    
    return true;
}

void audiorcv_task(void)
{
    tud_task();
    terminal_task();
    led_blinking_task();
#if CFG_AUDIO_DEBUG
    audio_debug_task();
#endif
    audio_task();
}

//--------------------------------------------------------------------+
// Device callbacks
//--------------------------------------------------------------------+

// Invoked when device is mounted
void tud_mount_cb(void) {
  blink_interval_ms = BLINK_MOUNTED;
}

// Invoked when device is unmounted
void tud_umount_cb(void) {
  blink_interval_ms = BLINK_NOT_MOUNTED;
  usb_audio_streaming = false;
}

// Invoked when usb bus is suspended
// remote_wakeup_en : if host allow us  to perform remote wakeup
// Within 7ms, device must draw an average of current less than 2.5 mA from bus
void tud_suspend_cb(bool remote_wakeup_en) {
  (void) remote_wakeup_en;
  blink_interval_ms = BLINK_SUSPENDED;
}

// Invoked when usb bus is resumed
void tud_resume_cb(void) {
  blink_interval_ms = tud_mounted() ? BLINK_MOUNTED : BLINK_NOT_MOUNTED;
}

//--------------------------------------------------------------------+
// Application Callback API Implementations
//--------------------------------------------------------------------+

//--------------------------------------------------------------------+
// UAC1 Helper Functions
//--------------------------------------------------------------------+

static bool audio10_set_req_ep(tusb_control_request_t const *p_request, uint8_t *pBuff) {
  uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);

  switch (ctrlSel) {
    case AUDIO10_EP_CTRL_SAMPLING_FREQ:
      if (p_request->bRequest == AUDIO10_CS_REQ_SET_CUR) {
        // Request uses 3 bytes
        TU_VERIFY(p_request->wLength == 3);

        current_sample_rate = tu_unaligned_read32(pBuff) & 0x00FFFFFF;

        TU_LOG2("EP set current freq: %" PRIu32 "\r\n", current_sample_rate);

        return true;
      }
      break;

    // Unknown/Unsupported control
    default:
      TU_BREAKPOINT();
      return false;
  }

  return false;
}

static bool audio10_get_req_ep(uint8_t rhport, tusb_control_request_t const *p_request) {
  uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);

  switch (ctrlSel) {
    case AUDIO10_EP_CTRL_SAMPLING_FREQ:
      if (p_request->bRequest == AUDIO10_CS_REQ_GET_CUR) {
        TU_LOG2("EP get current freq\r\n");

        uint8_t freq[3];
        freq[0] = (uint8_t) (current_sample_rate & 0xFF);
        freq[1] = (uint8_t) ((current_sample_rate >> 8) & 0xFF);
        freq[2] = (uint8_t) ((current_sample_rate >> 16) & 0xFF);
        return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, freq, sizeof(freq));
      }
      break;

    // Unknown/Unsupported control
    default:
      TU_BREAKPOINT();
      return false;
  }

  return false;
}

static bool audio10_set_req_entity(tusb_control_request_t const *p_request, uint8_t *pBuff) {
  uint8_t channelNum = TU_U16_LOW(p_request->wValue);
  uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
  uint8_t entityID = TU_U16_HIGH(p_request->wIndex);

  // If request is for our feature unit
  if (entityID == UAC1_ENTITY_FEATURE_UNIT) {
    switch (ctrlSel) {
      case AUDIO10_FU_CTRL_MUTE:
        switch (p_request->bRequest) {
          case AUDIO10_CS_REQ_SET_CUR:
            // Only 1st form is supported
            TU_VERIFY(p_request->wLength == 1);

            mute[channelNum] = pBuff[0];

            TU_LOG2("    Set Mute: %d of channel: %u\r\n", mute[channelNum], channelNum);
            return true;

          default:
            return false; // not supported
        }

      case AUDIO10_FU_CTRL_VOLUME:
        switch (p_request->bRequest) {
          case AUDIO10_CS_REQ_SET_CUR:
            // Only 1st form is supported
            TU_VERIFY(p_request->wLength == 2);

            volume[channelNum] = (int16_t)tu_unaligned_read16(pBuff) / 256;

            TU_LOG2("    Set Volume: %d dB of channel: %u\r\n", volume[channelNum], channelNum);
            return true;

          default:
            return false; // not supported
        }

        // Unknown/Unsupported control
      default:
        TU_BREAKPOINT();
        return false;
    }
  }

  return false;
}

static bool audio10_get_req_entity(uint8_t rhport, tusb_control_request_t const *p_request) {
  uint8_t channelNum = TU_U16_LOW(p_request->wValue);
  uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
  uint8_t entityID = TU_U16_HIGH(p_request->wIndex);

  // If request is for our feature unit
  if (entityID == UAC1_ENTITY_FEATURE_UNIT) {
    switch (ctrlSel) {
      case AUDIO10_FU_CTRL_MUTE:
        // Audio control mute cur parameter block consists of only one byte - we thus can send it right away
        // There does not exist a range parameter block for mute
        TU_LOG2("    Get Mute of channel: %u\r\n", channelNum);
        return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &mute[channelNum], 1);

      case AUDIO10_FU_CTRL_VOLUME:
        switch (p_request->bRequest) {
          case AUDIO10_CS_REQ_GET_CUR:
            TU_LOG2("    Get Volume of channel: %u\r\n", channelNum);
            {
              int16_t vol = (int16_t) volume[channelNum];
              vol = vol * 256; // convert to 1/256 dB units
              return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &vol, sizeof(vol));
            }

          case AUDIO10_CS_REQ_GET_MIN:
            TU_LOG2("    Get Volume min of channel: %u\r\n", channelNum);
            {
              int16_t min = -90; // -90 dB
              min = min * 256; // convert to 1/256 dB units
              return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &min, sizeof(min));
            }

          case AUDIO10_CS_REQ_GET_MAX:
            TU_LOG2("    Get Volume max of channel: %u\r\n", channelNum);
            {
              int16_t max = 30; // +30 dB
              max = max * 256; // convert to 1/256 dB units
              return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &max, sizeof(max));
            }

          case AUDIO10_CS_REQ_GET_RES:
            TU_LOG2("    Get Volume res of channel: %u\r\n", channelNum);
            {
              int16_t res = 128; // 0.5 dB
              return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &res, sizeof(res));
            }
            // Unknown/Unsupported control
          default:
            TU_BREAKPOINT();
            return false;
        }
        break;

        // Unknown/Unsupported control
      default:
        TU_BREAKPOINT();
        return false;
    }
  }

  return false;
}

//--------------------------------------------------------------------+
// UAC2 Helper Functions
//--------------------------------------------------------------------+

#if TUD_OPT_HIGH_SPEED
// List of supported sample rates for UAC2
const uint32_t sample_rates[] = {44100, 48000, 88200, 96000};

#define N_SAMPLE_RATES TU_ARRAY_SIZE(sample_rates)

static bool audio20_clock_get_request(uint8_t rhport, audio20_control_request_t const *request) {
  TU_ASSERT(request->bEntityID == UAC2_ENTITY_CLOCK);

  if (request->bControlSelector == AUDIO20_CS_CTRL_SAM_FREQ) {
    if (request->bRequest == AUDIO20_CS_REQ_CUR) {
      TU_LOG1("Clock get current freq %" PRIu32 "\r\n", current_sample_rate);

      audio20_control_cur_4_t curf = {(int32_t) tu_htole32(current_sample_rate)};
      return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *) request, &curf, sizeof(curf));
    } else if (request->bRequest == AUDIO20_CS_REQ_RANGE) {
      audio20_control_range_4_n_t(N_SAMPLE_RATES) rangef =
          {
              .wNumSubRanges = tu_htole16(N_SAMPLE_RATES)};
      TU_LOG1("Clock get %d freq ranges\r\n", N_SAMPLE_RATES);
      for (uint8_t i = 0; i < N_SAMPLE_RATES; i++) {
        rangef.subrange[i].bMin = (int32_t) sample_rates[i];
        rangef.subrange[i].bMax = (int32_t) sample_rates[i];
        rangef.subrange[i].bRes = 0;
        TU_LOG1("Range %d (%d, %d, %d)\r\n", i, (int) rangef.subrange[i].bMin, (int) rangef.subrange[i].bMax, (int) rangef.subrange[i].bRes);
      }

      return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *) request, &rangef, sizeof(rangef));
    }
  } else if (request->bControlSelector == AUDIO20_CS_CTRL_CLK_VALID &&
             request->bRequest == AUDIO20_CS_REQ_CUR) {
    audio20_control_cur_1_t cur_valid = {.bCur = 1};
    TU_LOG1("Clock get is valid %u\r\n", cur_valid.bCur);
    return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *) request, &cur_valid, sizeof(cur_valid));
  }
  TU_LOG1("Clock get request not supported, entity = %u, selector = %u, request = %u\r\n",
          request->bEntityID, request->bControlSelector, request->bRequest);
  return false;
}

static bool audio20_clock_set_request(audio20_control_request_t const *request, uint8_t const *buf) {
  TU_ASSERT(request->bEntityID == UAC2_ENTITY_CLOCK);
  TU_VERIFY(request->bRequest == AUDIO20_CS_REQ_CUR);

  if (request->bControlSelector == AUDIO20_CS_CTRL_SAM_FREQ) {
    TU_VERIFY(request->wLength == sizeof(audio20_control_cur_4_t));

    current_sample_rate = (uint32_t) ((audio20_control_cur_4_t const *) buf)->bCur;

    TU_LOG1("Clock set current freq: %" PRIu32 "\r\n", current_sample_rate);

    return true;
  } else {
    TU_LOG1("Clock set request not supported, entity = %u, selector = %u, request = %u\r\n",
            request->bEntityID, request->bControlSelector, request->bRequest);
    return false;
  }
}

static bool audio20_feature_unit_get_request(uint8_t rhport, audio20_control_request_t const *request) {
  TU_ASSERT(request->bEntityID == UAC2_ENTITY_FEATURE_UNIT);

  if (request->bControlSelector == AUDIO20_FU_CTRL_MUTE && request->bRequest == AUDIO20_CS_REQ_CUR) {
    audio20_control_cur_1_t mute1 = {.bCur = mute[request->bChannelNumber]};
    TU_LOG1("Get channel %u mute %d\r\n", request->bChannelNumber, mute1.bCur);
    return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *) request, &mute1, sizeof(mute1));
  } else if (request->bControlSelector == AUDIO20_FU_CTRL_VOLUME) {
    if (request->bRequest == AUDIO20_CS_REQ_RANGE) {
      audio20_control_range_2_n_t(1) range_vol = {
          .wNumSubRanges = tu_htole16(1),
          .subrange[0] = {.bMin = tu_htole16(-VOLUME_CTRL_50_DB), tu_htole16(VOLUME_CTRL_0_DB), tu_htole16(256)}};
      TU_LOG1("Get channel %u volume range (%d, %d, %u) dB\r\n", request->bChannelNumber,
              range_vol.subrange[0].bMin / 256, range_vol.subrange[0].bMax / 256, range_vol.subrange[0].bRes / 256);
      return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *) request, &range_vol, sizeof(range_vol));
    } else if (request->bRequest == AUDIO20_CS_REQ_CUR) {
      audio20_control_cur_2_t cur_vol = {.bCur = tu_htole16(volume[request->bChannelNumber])};
      TU_LOG1("Get channel %u volume %d dB\r\n", request->bChannelNumber, cur_vol.bCur / 256);
      return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *) request, &cur_vol, sizeof(cur_vol));
    }
  }
  TU_LOG1("Feature unit get request not supported, entity = %u, selector = %u, request = %u\r\n",
          request->bEntityID, request->bControlSelector, request->bRequest);

  return false;
}

static bool audio20_feature_unit_set_request(audio20_control_request_t const *request, uint8_t const *buf) {
  TU_ASSERT(request->bEntityID == UAC2_ENTITY_FEATURE_UNIT);
  TU_VERIFY(request->bRequest == AUDIO20_CS_REQ_CUR);

  if (request->bControlSelector == AUDIO20_FU_CTRL_MUTE) {
    TU_VERIFY(request->wLength == sizeof(audio20_control_cur_1_t));

    mute[request->bChannelNumber] = ((audio20_control_cur_1_t const *) buf)->bCur;

    TU_LOG1("Set channel %d Mute: %d\r\n", request->bChannelNumber, mute[request->bChannelNumber]);

    return true;
  } else if (request->bControlSelector == AUDIO20_FU_CTRL_VOLUME) {
    TU_VERIFY(request->wLength == sizeof(audio20_control_cur_2_t));

    volume[request->bChannelNumber] = ((audio20_control_cur_2_t const *) buf)->bCur;

    TU_LOG1("Set channel %d volume: %d dB\r\n", request->bChannelNumber, volume[request->bChannelNumber] / 256);

    return true;
  } else {
    TU_LOG1("Feature unit set request not supported, entity = %u, selector = %u, request = %u\r\n",
            request->bEntityID, request->bControlSelector, request->bRequest);
    return false;
  }
}

static bool audio20_get_req_entity(uint8_t rhport, tusb_control_request_t const *p_request) {
  audio20_control_request_t const *request = (audio20_control_request_t const *) p_request;

  if (request->bEntityID == UAC2_ENTITY_CLOCK)
    return audio20_clock_get_request(rhport, request);
  if (request->bEntityID == UAC2_ENTITY_FEATURE_UNIT)
    return audio20_feature_unit_get_request(rhport, request);
  else {
    TU_LOG1("Get request not handled, entity = %d, selector = %d, request = %d\r\n",
            request->bEntityID, request->bControlSelector, request->bRequest);
  }
  return false;
}

static bool audio20_set_req_entity(tusb_control_request_t const *p_request, uint8_t *buf) {
  audio20_control_request_t const *request = (audio20_control_request_t const *) p_request;

  if (request->bEntityID == UAC2_ENTITY_FEATURE_UNIT)
    return audio20_feature_unit_set_request(request, buf);
  if (request->bEntityID == UAC2_ENTITY_CLOCK)
    return audio20_clock_set_request(request, buf);
  TU_LOG1("Set request not handled, entity = %d, selector = %d, request = %d\r\n",
          request->bEntityID, request->bControlSelector, request->bRequest);

  return false;
}

#endif // TUD_OPT_HIGH_SPEED

//--------------------------------------------------------------------+
// Main Callback Functions
//--------------------------------------------------------------------+

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
  (void) rhport;
  uint8_t const itf = tu_u16_low(tu_le16toh(p_request->wIndex));
  uint8_t const alt = tu_u16_low(tu_le16toh(p_request->wValue));

  TU_LOG2("Set interface %d alt %d\r\n", itf, alt);
  if (ITF_NUM_AUDIO_STREAMING == itf && alt != 0) {
    blink_interval_ms = BLINK_STREAMING;
    usb_audio_streaming = true;
  }

#if CFG_AUDIO_DEBUG
  current_alt_settings = alt;
#endif

  return true;
}

// Invoked when audio class specific set request received for an EP
bool tud_audio_set_req_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *pBuff) {
  (void) rhport;
  (void) pBuff;

  if (tud_audio_version() == 1) {
    return audio10_set_req_ep(p_request, pBuff);
  } else if (tud_audio_version() == 2) {
    // We do not support any requests here
  }

  return false;// Yet not implemented
}

// Invoked when audio class specific get request received for an EP
bool tud_audio_get_req_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
  (void) rhport;

  if (tud_audio_version() == 1) {
    return audio10_get_req_ep(rhport, p_request);
  } else if (tud_audio_version() == 2) {
    // We do not support any requests here
  }

  return false;// Yet not implemented
}

// Invoked when audio class specific set request received for an entity
bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *buf) {
  (void) rhport;

  if (tud_audio_version() == 1) {
    return audio10_set_req_entity(p_request, buf);
#if TUD_OPT_HIGH_SPEED
  } else if (tud_audio_version() == 2) {
    return audio20_set_req_entity(p_request, buf);
#endif
  }

  return false;
}

// Invoked when audio class specific get request received for an entity
bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
  (void) rhport;

  if (tud_audio_version() == 1) {
    return audio10_get_req_entity(rhport, p_request);
#if TUD_OPT_HIGH_SPEED
  } else if (tud_audio_version() == 2) {
    return audio20_get_req_entity(rhport, p_request);
#endif
  }

  return false;
}

bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
  (void) rhport;

  uint8_t const itf = tu_u16_low(tu_le16toh(p_request->wIndex));
  uint8_t const alt = tu_u16_low(tu_le16toh(p_request->wValue));

  if (ITF_NUM_AUDIO_STREAMING == itf && alt == 0) {
    blink_interval_ms = BLINK_MOUNTED;
    usb_audio_streaming = false;
  }

  return true;
}

void tud_audio_feedback_params_cb(uint8_t func_id, uint8_t alt_itf, audio_feedback_params_t *feedback_param) {
  (void) func_id;
  (void) alt_itf;
  // Set feedback method to fifo counting
  feedback_param->method = AUDIO_FEEDBACK_METHOD_FIFO_COUNT;
  feedback_param->sample_freq = current_sample_rate;

  // About FIFO threshold:
  //
  // By default the threshold is set to half FIFO size, which works well in most cases,
  // you can reduce the threshold to have less latency.
  //
  // For example, here we could set the threshold to 2 ms of audio data, as audio_task() read audio data every 1 ms,
  // having 2 ms threshold allows some margin and a quick response:
  //
  // feedback_param->fifo_count.fifo_threshold =
  //    current_sample_rate * CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX * CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_RX / 1000 * 2;
}

#if CFG_AUDIO_DEBUG
bool tud_audio_rx_done_isr(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id, uint8_t ep_out, uint8_t cur_alt_setting) {
  (void) rhport;
  (void) n_bytes_received;
  (void) func_id;
  (void) ep_out;
  (void) cur_alt_setting;

  fifo_count = tud_audio_available();
  // Same averaging method used in UAC2 class
  fifo_count_avg = (uint32_t) (((uint64_t) fifo_count_avg * 63 + ((uint32_t) fifo_count << 16)) >> 6);

  return true;
}
#endif

//--------------------------------------------------------------------+
// CDC Task
//--------------------------------------------------------------------+

void cdc_task(void)
{
    bool connected = tud_cdc_connected();

    if (!connected)
    {
        return;
    }

    /*
     * 接続後、キャッシュが空になるまで少しずつ送る。
     */
    if (cdc_startup_cache_len > 0)
    {
        cdc_flush_startup_cache();
    }

    /*
     * RX
     */
    while (tud_cdc_available())
    {
        uint8_t buffer[64];

        uint32_t count = tud_cdc_read(
            buffer,
            sizeof(buffer)
        );

        if (count == 0)
        {
            break;
        }

        uint32_t available = tud_cdc_write_available();

        if (count > available)
        {
            count = available;
        }

        tud_cdc_write(
            buffer,
            count
        );
    }
    tud_cdc_write_flush();
}

//--------------------------------------------------------------------+
// AUDIO Task
//--------------------------------------------------------------------+

// This task simulates an audio transmit callback, one frame is sent every 1ms.
// In a real application, this would be replaced with actual I2S transmit callback.
void audio_task(void)
{
    static uint32_t start_ms = 0;

    static int16_t usb_pcm[48 * 2];

    uint32_t curr_ms =
        audiorcv_millis();

    if (start_ms == curr_ms)
    {
        return;
    }

    start_ms = curr_ms;

    const uint16_t request_bytes =
        48u *
        2u *
        sizeof(int16_t);

    uint16_t received =
        tud_audio_read(
            usb_pcm,
            request_bytes
        );

    size_t frames =
        received /
        (2u * sizeof(int16_t));

    for (size_t i = 0; i < frames; i++)
    {
        uint32_t w =
            audio_write_index;

        uint32_t r =
            audio_read_index;

        /*
         * buffer full
         */
        if ((w - r) >= AUDIO_RING_FRAMES)
        {
            audio_ring_overflow_count++;
            break;
        }

        audio_ring[w & AUDIO_RING_MASK].left =
            usb_pcm[i * 2u + 0u];

        audio_ring[w & AUDIO_RING_MASK].right =
            usb_pcm[i * 2u + 1u];

        audio_write_index =
            w + 1u;
    }
}

//--------------------------------------------------------------------+
// BLINKING TASK
//--------------------------------------------------------------------+
void led_blinking_task(void) {
  static uint32_t start_ms = 0;
  static bool led_state = false;

  // Blink every interval ms
  if (audiorcv_millis() - start_ms < blink_interval_ms) return;
  start_ms += blink_interval_ms;

  audiorcv_led_write(led_state);
  led_state = 1 - led_state;
}

#if CFG_AUDIO_DEBUG
//--------------------------------------------------------------------+
// HID interface for audio debug
//--------------------------------------------------------------------+
// Every 1ms, we will sent 1 debug information report
void audio_debug_task(void) {
  static uint32_t start_ms = 0;
  uint32_t curr_ms = audiorcv_millis();
  if (start_ms == curr_ms) return;// not enough time
  start_ms = curr_ms;

  audio_debug_info_t debug_info;
  debug_info.sample_rate = current_sample_rate;
  debug_info.alt_settings = current_alt_settings;
  debug_info.fifo_size = CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ;
  debug_info.fifo_count = fifo_count;
  debug_info.fifo_count_avg = (uint16_t) (fifo_count_avg >> 16);
  for (int i = 0; i < CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1; i++) {
    debug_info.mute[i] = mute[i];
    debug_info.volume[i] = volume[i];
  }

  if (tud_hid_ready())
    tud_hid_report(0, &debug_info, sizeof(debug_info));
}

// Invoked when received GET_REPORT control request
// Unused here
uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen) {
  // TODO not Implemented
  (void) itf;
  (void) report_id;
  (void) report_type;
  (void) buffer;
  (void) reqlen;

  return 0;
}

// Invoked when received SET_REPORT control request or
// Unused here
void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize) {
  // This example doesn't use multiple report and report ID
  (void) itf;
  (void) report_id;
  (void) report_type;
  (void) buffer;
  (void) bufsize;
}

#endif

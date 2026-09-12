#ifndef AUDIORCV_H
#define AUDIORCV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
    int16_t left;
    int16_t right;
} audio_stereo_frame_t;

bool audiorcv_init(void);
void audiorcv_task(void);
bool audiorcv_usb_mounted(void);
bool audiorcv_usb_streaming(void);
uint32_t audiorcv_sample_rate(void);
uint32_t audiorcv_ring_count(void);
uint32_t audiorcv_ring_capacity(void);
uint32_t audiorcv_ring_overflows(void);
uint32_t audiorcv_ring_underflows(void);
void audiorcv_set_network_consumer(bool enabled);
size_t audiorcv_read_frames(audio_stereo_frame_t *frames, size_t count);

#endif

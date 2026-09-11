#ifndef SCREEN_SHARING_CAPTURE_H
#define SCREEN_SHARING_CAPTURE_H

#include <stdint.h>

typedef struct ss_frame {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint8_t *pixels;
} ss_frame_t;

int ss_capture_primary_screen(ss_frame_t *frame);
void ss_frame_release(ss_frame_t *frame);
int ss_frame_copy_region(const ss_frame_t *source, uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint8_t *destination, uint32_t destination_stride);

#endif

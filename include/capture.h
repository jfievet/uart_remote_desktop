#ifndef SCREEN_SHARING_CAPTURE_H
#define SCREEN_SHARING_CAPTURE_H

#include <stdint.h>

typedef struct ss_frame {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint8_t *pixels;
} ss_frame_t;

typedef enum ss_capture_backend {
    SS_CAPTURE_BACKEND_GDI = 0,
    SS_CAPTURE_BACKEND_DXGI = 1
} ss_capture_backend_t;

void ss_capture_set_backend(ss_capture_backend_t backend);
int ss_capture_parse_backend(const char *name, ss_capture_backend_t *out_backend);
int ss_capture_primary_screen(ss_frame_t *frame);
void ss_frame_release(ss_frame_t *frame);
int ss_frame_copy_region(const ss_frame_t *source, uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint8_t *destination, uint32_t destination_stride);
void ss_capture_shutdown(void);

#endif

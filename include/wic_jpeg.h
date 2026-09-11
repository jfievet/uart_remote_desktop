#ifndef SCREEN_SHARING_WIC_JPEG_H
#define SCREEN_SHARING_WIC_JPEG_H

#include <stddef.h>
#include <stdint.h>

int ss_jpeg_encode_bgra(const uint8_t *bgra, uint32_t width, uint32_t height, uint32_t stride, float quality, uint8_t **out_data, size_t *out_size);
int ss_jpeg_decode_to_bgra(const uint8_t *data, size_t size, uint8_t **out_pixels, uint32_t *out_width, uint32_t *out_height, uint32_t *out_stride);
void ss_jpeg_shutdown(void);

#endif

#define WIN32_LEAN_AND_MEAN

#include "wic_jpeg.h"

#include <windows.h>
#include <objbase.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void fill_pattern(uint8_t *pixels, uint32_t width, uint32_t height, uint32_t stride)
{
    uint32_t y;

    for (y = 0; y < height; ++y) {
        uint32_t x;
        uint8_t *row = pixels + (size_t)y * stride;

        for (x = 0; x < width; ++x) {
            row[x * 4u + 0u] = (uint8_t)(x * 11u + y * 3u);
            row[x * 4u + 1u] = (uint8_t)(x * 5u + y * 7u);
            row[x * 4u + 2u] = (uint8_t)(x * 13u + y * 17u);
            row[x * 4u + 3u] = 255u;
        }
    }
}

static int run_backend(ss_jpeg_backend_t backend, const char *name)
{
    const uint32_t width = 16;
    const uint32_t height = 16;
    const uint32_t stride = width * 4u;
    uint8_t source[16u * 16u * 4u];
    uint8_t *encoded = NULL;
    size_t encoded_size = 0;
    uint8_t *decoded = NULL;
    uint32_t decoded_width = 0;
    uint32_t decoded_height = 0;
    uint32_t decoded_stride = 0;
    int result = 1;

    fill_pattern(source, width, height, stride);
    ss_jpeg_set_backend(backend);

    if (ss_jpeg_encode_bgra(source, width, height, stride, 0.80f, &encoded, &encoded_size) != 0 || encoded == NULL || encoded_size == 0) {
        printf("%s encode failed\n", name);
        goto cleanup;
    }

    if (ss_jpeg_decode_to_bgra(encoded, encoded_size, &decoded, &decoded_width, &decoded_height, &decoded_stride) != 0 || decoded == NULL) {
        printf("%s decode failed\n", name);
        goto cleanup;
    }

    if (decoded_width != width || decoded_height != height || decoded_stride != stride) {
        printf("%s dimensions changed: %ux%u stride %u\n", name, decoded_width, decoded_height, decoded_stride);
        goto cleanup;
    }

    result = 0;

cleanup:
    free(decoded);
    free(encoded);
    ss_jpeg_shutdown();
    return result;
}

int main(void)
{
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    int result;

    if (FAILED(hr)) {
        printf("COM initialization failed\n");
        return 1;
    }

    result = run_backend(SS_JPEG_BACKEND_WINDOWS, "windows");
    if (result == 0) {
        result = run_backend(SS_JPEG_BACKEND_C, "c");
    }

    CoUninitialize();
    return result;
}

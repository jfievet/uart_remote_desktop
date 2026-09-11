#define WIN32_LEAN_AND_MEAN

#include "capture.h"

#include <windows.h>
#include <stdlib.h>

void ss_frame_release(ss_frame_t *frame)
{
    if (frame == NULL) {
        return;
    }

    free(frame->pixels);
    frame->pixels = NULL;
    frame->width = 0;
    frame->height = 0;
    frame->stride = 0;
}

int ss_capture_primary_screen(ss_frame_t *frame)
{
    HDC screen_dc = NULL;
    HDC memory_dc = NULL;
    HBITMAP bitmap = NULL;
    HGDIOBJ old_object = NULL;
    void *bitmap_bits = NULL;
    BITMAPINFO bitmap_info;
    int width = GetSystemMetrics(SM_CXSCREEN);
    int height = GetSystemMetrics(SM_CYSCREEN);
    size_t buffer_size;
    int success = 0;

    ZeroMemory(frame, sizeof(*frame));
    ZeroMemory(&bitmap_info, sizeof(bitmap_info));

    if (width <= 0 || height <= 0) {
        return -1;
    }

    screen_dc = GetDC(NULL);
    if (screen_dc == NULL) {
        return -1;
    }

    memory_dc = CreateCompatibleDC(screen_dc);
    if (memory_dc == NULL) {
        ReleaseDC(NULL, screen_dc);
        return -1;
    }

    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = width;
    bitmap_info.bmiHeader.biHeight = -height;
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;

    bitmap = CreateDIBSection(screen_dc, &bitmap_info, DIB_RGB_COLORS, &bitmap_bits, NULL, 0);
    if (bitmap == NULL || bitmap_bits == NULL) {
        DeleteDC(memory_dc);
        ReleaseDC(NULL, screen_dc);
        return -1;
    }

    old_object = SelectObject(memory_dc, bitmap);
    if (old_object == NULL) {
        DeleteObject(bitmap);
        DeleteDC(memory_dc);
        ReleaseDC(NULL, screen_dc);
        return -1;
    }

    if (!BitBlt(memory_dc, 0, 0, width, height, screen_dc, 0, 0, SRCCOPY | CAPTUREBLT)) {
        SelectObject(memory_dc, old_object);
        DeleteObject(bitmap);
        DeleteDC(memory_dc);
        ReleaseDC(NULL, screen_dc);
        return -1;
    }

    buffer_size = (size_t)width * (size_t)height * 4u;
    frame->pixels = (uint8_t *)malloc(buffer_size);
    if (frame->pixels == NULL) {
        SelectObject(memory_dc, old_object);
        DeleteObject(bitmap);
        DeleteDC(memory_dc);
        ReleaseDC(NULL, screen_dc);
        return -1;
    }

    memcpy(frame->pixels, bitmap_bits, buffer_size);
    {
        size_t alpha_index; /* BitBlt leaves alpha undefined; force opaque for JPEG */
        for (alpha_index = 3; alpha_index < buffer_size; alpha_index += 4) {
            frame->pixels[alpha_index] = 0xFF;
        }
    }
    frame->width = (uint32_t)width;
    frame->height = (uint32_t)height;
    frame->stride = (uint32_t)width * 4u;
    success = 0;

    SelectObject(memory_dc, old_object);
    DeleteObject(bitmap);
    DeleteDC(memory_dc);
    ReleaseDC(NULL, screen_dc);

    return success;
}

int ss_frame_copy_region(const ss_frame_t *source, uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint8_t *destination, uint32_t destination_stride)
{
    uint32_t row;

    if (source == NULL || source->pixels == NULL || destination == NULL) {
        return -1;
    }

    if (x >= source->width || y >= source->height) {
        return -1;
    }

    if (x + width > source->width) {
        width = source->width - x;
    }

    if (y + height > source->height) {
        height = source->height - y;
    }

    for (row = 0; row < height; ++row) {
        const uint8_t *source_row = source->pixels + (size_t)(y + row) * source->stride + (size_t)x * 4u;
        uint8_t *destination_row = destination + (size_t)row * destination_stride;
        memcpy(destination_row, source_row, (size_t)width * 4u);
    }

    return 0;
}

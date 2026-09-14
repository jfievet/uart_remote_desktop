#define WIN32_LEAN_AND_MEAN

#include "capture.h"
#include "capture_dxgi.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* only the capture thread ever calls into this module, so module-static state needs no locking */
static ss_capture_backend_t g_capture_backend = SS_CAPTURE_BACKEND_GDI;
static int g_dxgi_unavailable = 0;

typedef struct ss_gdi_capture_state {
    HDC memory_dc;
    HBITMAP bitmap;
    void *bitmap_bits;
    int width;
    int height;
} ss_gdi_capture_state_t;

static ss_gdi_capture_state_t g_gdi_state;

void ss_capture_set_backend(ss_capture_backend_t backend)
{
    g_capture_backend = backend;
}

int ss_capture_parse_backend(const char *name, ss_capture_backend_t *out_backend)
{
    if (name == NULL || out_backend == NULL) {
        return -1;
    }

    if (strcmp(name, "gdi") == 0) {
        *out_backend = SS_CAPTURE_BACKEND_GDI;
        return 0;
    }
    if (strcmp(name, "dxgi") == 0) {
        *out_backend = SS_CAPTURE_BACKEND_DXGI;
        return 0;
    }

    return -1;
}

static void ss_gdi_capture_state_release(ss_gdi_capture_state_t *state)
{
    if (state->bitmap != NULL) {
        DeleteObject(state->bitmap);
        state->bitmap = NULL;
    }
    if (state->memory_dc != NULL) {
        DeleteDC(state->memory_dc);
        state->memory_dc = NULL;
    }
    state->bitmap_bits = NULL;
    state->width = 0;
    state->height = 0;
}

/* (re)creates the cached DC/DIB section only when missing or when the screen resolution changed */
static int ss_gdi_capture_state_ensure(ss_gdi_capture_state_t *state, HDC screen_dc, int width, int height)
{
    BITMAPINFO bitmap_info;

    if (state->memory_dc != NULL && state->bitmap != NULL && state->width == width && state->height == height) {
        return 0;
    }

    ss_gdi_capture_state_release(state);

    state->memory_dc = CreateCompatibleDC(screen_dc);
    if (state->memory_dc == NULL) {
        return -1;
    }

    ZeroMemory(&bitmap_info, sizeof(bitmap_info));
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = width;
    bitmap_info.bmiHeader.biHeight = -height;
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;

    state->bitmap = CreateDIBSection(screen_dc, &bitmap_info, DIB_RGB_COLORS, &state->bitmap_bits, NULL, 0);
    if (state->bitmap == NULL || state->bitmap_bits == NULL) {
        ss_gdi_capture_state_release(state);
        return -1;
    }

    if (SelectObject(state->memory_dc, state->bitmap) == NULL) {
        ss_gdi_capture_state_release(state);
        return -1;
    }

    state->width = width;
    state->height = height;
    return 0;
}

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

static int ss_capture_primary_screen_gdi(ss_frame_t *frame)
{
    HDC screen_dc;
    int width = GetSystemMetrics(SM_CXSCREEN);
    int height = GetSystemMetrics(SM_CYSCREEN);
    size_t buffer_size;

    if (width <= 0 || height <= 0) {
        return -1;
    }

    screen_dc = GetDC(NULL);
    if (screen_dc == NULL) {
        return -1;
    }

    if (ss_gdi_capture_state_ensure(&g_gdi_state, screen_dc, width, height) != 0) {
        ReleaseDC(NULL, screen_dc);
        return -1;
    }

    if (!BitBlt(g_gdi_state.memory_dc, 0, 0, width, height, screen_dc, 0, 0, SRCCOPY | CAPTUREBLT)) {
        ReleaseDC(NULL, screen_dc);
        return -1;
    }
    ReleaseDC(NULL, screen_dc);

    buffer_size = (size_t)width * (size_t)height * 4u;
    frame->pixels = (uint8_t *)malloc(buffer_size);
    if (frame->pixels == NULL) {
        return -1;
    }

    memcpy(frame->pixels, g_gdi_state.bitmap_bits, buffer_size);
    {
        size_t alpha_index; /* BitBlt leaves alpha undefined; force opaque for JPEG */
        for (alpha_index = 3; alpha_index < buffer_size; alpha_index += 4) {
            frame->pixels[alpha_index] = 0xFF;
        }
    }

    frame->width = (uint32_t)width;
    frame->height = (uint32_t)height;
    frame->stride = (uint32_t)width * 4u;
    return 0;
}

int ss_capture_primary_screen(ss_frame_t *frame)
{
    ZeroMemory(frame, sizeof(*frame));

    if (g_capture_backend == SS_CAPTURE_BACKEND_DXGI && !g_dxgi_unavailable) {
        if (ss_capture_dxgi_capture(frame) == 0) {
            return 0;
        }

        /* first DXGI failure permanently falls back to GDI for the rest of the process */
        g_dxgi_unavailable = 1;
        ss_capture_dxgi_shutdown();
        printf("DXGI capture unavailable, falling back to GDI capture.\n");
        fflush(stdout);
    }

    return ss_capture_primary_screen_gdi(frame);
}

void ss_capture_shutdown(void)
{
    ss_gdi_capture_state_release(&g_gdi_state);
    ss_capture_dxgi_shutdown();
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


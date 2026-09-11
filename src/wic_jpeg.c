#define WIN32_LEAN_AND_MEAN

#include "wic_jpeg.h"

#include <windows.h>
#include <ole2.h>
#include <gdiplus/gdiplus.h>
#include <gdiplus/gdiplusflat.h>
#include <gdiplus/gdiplusinit.h>
#include <gdiplus/gdiplusimaging.h>
#include <gdiplus/gdipluspixelformats.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static ULONG_PTR ss_gdiplus_token = 0;

static int ss_jpeg_startup(void)
{
    GdiplusStartupInput input;

    if (ss_gdiplus_token != 0) {
        return 0;
    }

    ZeroMemory(&input, sizeof(input));
    input.GdiplusVersion = 1;

    return GdiplusStartup(&ss_gdiplus_token, &input, NULL) == Ok ? 0 : -1;
}

void ss_jpeg_shutdown(void)
{
    if (ss_gdiplus_token != 0) {
        GdiplusShutdown(ss_gdiplus_token);
        ss_gdiplus_token = 0;
    }
}

static int ss_copy_stream_to_buffer(IStream *stream, uint8_t **out_data, size_t *out_size)
{
    STATSTG statistics;
    HGLOBAL global_handle;
    void *locked_data;
    SIZE_T size;
    uint8_t *buffer;

    if (stream->lpVtbl->Stat(stream, &statistics, STATFLAG_NONAME) != S_OK) {
        return -1;
    }

    if (statistics.cbSize.QuadPart == 0 || statistics.cbSize.QuadPart > (ULONGLONG)SIZE_MAX) {
        return -1;
    }

    if (GetHGlobalFromStream(stream, &global_handle) != S_OK) {
        return -1;
    }

    locked_data = GlobalLock(global_handle);
    if (locked_data == NULL) {
        return -1;
    }

    size = (SIZE_T)statistics.cbSize.QuadPart;
    buffer = (uint8_t *)malloc((size_t)size);
    if (buffer == NULL) {
        GlobalUnlock(global_handle);
        return -1;
    }

    memcpy(buffer, locked_data, (size_t)size);
    GlobalUnlock(global_handle);

    *out_data = buffer;
    *out_size = (size_t)size;
    return 0;
}

static int ss_find_jpeg_encoder(CLSID *out_clsid)
{
    UINT count = 0;
    UINT bytes = 0;
    ImageCodecInfo *encoders;
    UINT index;

    if (GdipGetImageEncodersSize(&count, &bytes) != Ok || count == 0 || bytes == 0) {
        return -1;
    }

    encoders = (ImageCodecInfo *)malloc(bytes);
    if (encoders == NULL) {
        return -1;
    }

    if (GdipGetImageEncoders(count, bytes, encoders) != Ok) {
        free(encoders);
        return -1;
    }

    for (index = 0; index < count; ++index) {
        if (encoders[index].MimeType != NULL && wcscmp(encoders[index].MimeType, L"image/jpeg") == 0) {
            *out_clsid = encoders[index].Clsid;
            free(encoders);
            return 0;
        }
    }

    free(encoders);
    return -1;
}

int ss_jpeg_encode_bgra(const uint8_t *bgra, uint32_t width, uint32_t height, uint32_t stride, float quality, uint8_t **out_data, size_t *out_size)
{
    GpBitmap *bitmap = NULL;
    IStream *stream = NULL;
    CLSID jpeg_clsid;
    EncoderParameters parameters;
    ULONG quality_value;
    int result = -1;

    *out_data = NULL;
    *out_size = 0;

    if (bgra == NULL || width == 0 || height == 0 || stride < width * 4u) {
        return -1;
    }

    if (ss_jpeg_startup() != 0 || ss_find_jpeg_encoder(&jpeg_clsid) != 0) {
        return -1;
    }

    if (GdipCreateBitmapFromScan0((INT)width, (INT)height, (INT)stride, PixelFormat32bppARGB, (BYTE *)bgra, &bitmap) != Ok) {
        return -1;
    }

    if (CreateStreamOnHGlobal(NULL, TRUE, &stream) != S_OK) {
        GdipDisposeImage((GpImage *)bitmap);
        return -1;
    }

    if (quality < 0.0f) {
        quality = 0.0f;
    } else if (quality > 1.0f) {
        quality = 1.0f;
    }

    quality_value = (ULONG)(quality * 100.0f + 0.5f);
    ZeroMemory(&parameters, sizeof(parameters));
    parameters.Count = 1;
    parameters.Parameter[0].Guid = EncoderQuality;
    parameters.Parameter[0].NumberOfValues = 1;
    parameters.Parameter[0].Type = EncoderParameterValueTypeLong;
    parameters.Parameter[0].Value = &quality_value;

    if (GdipSaveImageToStream((GpImage *)bitmap, stream, &jpeg_clsid, &parameters) == Ok) {
        result = ss_copy_stream_to_buffer(stream, out_data, out_size);
    }

    stream->lpVtbl->Release(stream);
    GdipDisposeImage((GpImage *)bitmap);
    return result;
}

int ss_jpeg_decode_to_bgra(const uint8_t *data, size_t size, uint8_t **out_pixels, uint32_t *out_width, uint32_t *out_height, uint32_t *out_stride)
{
    HGLOBAL global_handle = NULL;
    void *locked_data;
    IStream *stream = NULL;
    GpBitmap *bitmap = NULL;
    GpRect rect;
    BitmapData bitmap_data;
    UINT width;
    UINT height;
    uint32_t stride;
    uint8_t *pixels;
    uint32_t row;
    int result = -1;

    *out_pixels = NULL;
    *out_width = 0;
    *out_height = 0;
    *out_stride = 0;

    if (data == NULL || size == 0) {
        return -1;
    }

    if (ss_jpeg_startup() != 0) {
        return -1;
    }

    global_handle = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)size);
    if (global_handle == NULL) {
        return -1;
    }

    locked_data = GlobalLock(global_handle);
    if (locked_data == NULL) {
        GlobalFree(global_handle);
        return -1;
    }

    memcpy(locked_data, data, size);
    GlobalUnlock(global_handle);

    if (CreateStreamOnHGlobal(global_handle, TRUE, &stream) != S_OK) {
        GlobalFree(global_handle);
        return -1;
    }
    global_handle = NULL;

    if (GdipCreateBitmapFromStream(stream, &bitmap) != Ok) {
        stream->lpVtbl->Release(stream);
        return -1;
    }

    if (GdipGetImageWidth((GpImage *)bitmap, &width) != Ok || GdipGetImageHeight((GpImage *)bitmap, &height) != Ok || width == 0 || height == 0) {
        goto cleanup;
    }

    if (width > UINT32_MAX / 4u || height > UINT32_MAX / (width * 4u)) {
        goto cleanup;
    }

    stride = (uint32_t)width * 4u;
    pixels = (uint8_t *)malloc((size_t)stride * (size_t)height);
    if (pixels == NULL) {
        goto cleanup;
    }

    rect.X = 0;
    rect.Y = 0;
    rect.Width = (INT)width;
    rect.Height = (INT)height;
    ZeroMemory(&bitmap_data, sizeof(bitmap_data));

    if (GdipBitmapLockBits(bitmap, &rect, ImageLockModeRead, PixelFormat32bppARGB, &bitmap_data) != Ok) {
        free(pixels);
        goto cleanup;
    }

    for (row = 0; row < height; ++row) {
        const uint8_t *source_row;
        if (bitmap_data.Stride >= 0) {
            source_row = (const uint8_t *)bitmap_data.Scan0 + (size_t)row * (size_t)bitmap_data.Stride;
        } else {
            source_row = (const uint8_t *)bitmap_data.Scan0 + (size_t)(height - 1u - row) * (size_t)(-bitmap_data.Stride);
        }
        memcpy(pixels + (size_t)row * stride, source_row, stride);
    }

    GdipBitmapUnlockBits(bitmap, &bitmap_data);

    *out_pixels = pixels;
    *out_width = (uint32_t)width;
    *out_height = (uint32_t)height;
    *out_stride = stride;
    result = 0;

cleanup:
    GdipDisposeImage((GpImage *)bitmap);
    stream->lpVtbl->Release(stream);
    return result;
}

#define WIN32_LEAN_AND_MEAN

#include "wic_jpeg.h"

#include <windows.h>
#include <ole2.h>
#include <gdiplus/gdiplus.h>
#include <gdiplus/gdiplusflat.h>
#include <gdiplus/gdiplusinit.h>
#include <gdiplus/gdiplusimaging.h>
#include <gdiplus/gdipluspixelformats.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

static ss_jpeg_backend_t ss_jpeg_backend = SS_JPEG_BACKEND_WINDOWS;
static ULONG_PTR ss_gdiplus_token = 0;

void ss_jpeg_set_backend(ss_jpeg_backend_t backend)
{
    if (backend == SS_JPEG_BACKEND_C || backend == SS_JPEG_BACKEND_WINDOWS) {
        ss_jpeg_backend = backend;
    }
}

int ss_jpeg_parse_backend(const char *name, ss_jpeg_backend_t *out_backend)
{
    if (name == NULL || out_backend == NULL) {
        return -1;
    }

    if (strcmp(name, "windows") == 0 || strcmp(name, "gdiplus") == 0) {
        *out_backend = SS_JPEG_BACKEND_WINDOWS;
        return 0;
    }

    if (strcmp(name, "c") == 0 || strcmp(name, "stb") == 0) {
        *out_backend = SS_JPEG_BACKEND_C;
        return 0;
    }

    return -1;
}

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

static int ss_jpeg_windows_encode_bgra(const uint8_t *bgra, uint32_t width, uint32_t height, uint32_t stride, float quality, uint8_t **out_data, size_t *out_size)
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

static int ss_jpeg_windows_decode_to_bgra(const uint8_t *data, size_t size, uint8_t **out_pixels, uint32_t *out_width, uint32_t *out_height, uint32_t *out_stride)
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

typedef struct ss_memory_writer {
    uint8_t *data;
    size_t size;
    size_t capacity;
    int failed;
} ss_memory_writer_t;

static void ss_memory_writer_append(void *context, void *data, int size)
{
    ss_memory_writer_t *writer = (ss_memory_writer_t *)context;
    size_t requested;
    size_t new_capacity;
    uint8_t *new_data;

    if (writer->failed || size <= 0) {
        return;
    }

    requested = writer->size + (size_t)size;
    if (requested < writer->size) {
        writer->failed = 1;
        return;
    }

    if (requested > writer->capacity) {
        new_capacity = writer->capacity == 0 ? 4096u : writer->capacity;
        while (new_capacity < requested) {
            size_t doubled = new_capacity * 2u;
            if (doubled <= new_capacity) {
                writer->failed = 1;
                return;
            }
            new_capacity = doubled;
        }

        new_data = (uint8_t *)realloc(writer->data, new_capacity);
        if (new_data == NULL) {
            writer->failed = 1;
            return;
        }

        writer->data = new_data;
        writer->capacity = new_capacity;
    }

    memcpy(writer->data + writer->size, data, (size_t)size);
    writer->size = requested;
}

static int ss_jpeg_c_encode_bgra(const uint8_t *bgra, uint32_t width, uint32_t height, uint32_t stride, float quality, uint8_t **out_data, size_t *out_size)
{
    uint8_t *rgb;
    uint32_t row;
    ss_memory_writer_t writer;
    int quality_percent;

    *out_data = NULL;
    *out_size = 0;

    if (bgra == NULL || width == 0 || height == 0 || stride < width * 4u || width > (uint32_t)INT_MAX || height > (uint32_t)INT_MAX) {
        return -1;
    }

    if ((size_t)height > SIZE_MAX / (size_t)width || (size_t)width * (size_t)height > SIZE_MAX / 3u) {
        return -1;
    }

    rgb = (uint8_t *)malloc((size_t)width * (size_t)height * 3u);
    if (rgb == NULL) {
        return -1;
    }

    for (row = 0; row < height; ++row) {
        uint32_t column;
        const uint8_t *source_row = bgra + (size_t)row * stride;
        uint8_t *destination_row = rgb + (size_t)row * (size_t)width * 3u;

        for (column = 0; column < width; ++column) {
            destination_row[column * 3u + 0u] = source_row[column * 4u + 2u];
            destination_row[column * 3u + 1u] = source_row[column * 4u + 1u];
            destination_row[column * 3u + 2u] = source_row[column * 4u + 0u];
        }
    }

    if (quality < 0.0f) {
        quality = 0.0f;
    } else if (quality > 1.0f) {
        quality = 1.0f;
    }
    quality_percent = (int)(quality * 100.0f + 0.5f);
    if (quality_percent < 1) {
        quality_percent = 1;
    }

    ZeroMemory(&writer, sizeof(writer));
    if (!stbi_write_jpg_to_func(ss_memory_writer_append, &writer, (int)width, (int)height, 3, rgb, quality_percent) || writer.failed || writer.size == 0) {
        free(writer.data);
        free(rgb);
        return -1;
    }

    free(rgb);
    *out_data = writer.data;
    *out_size = writer.size;
    return 0;
}

static int ss_jpeg_c_decode_to_bgra(const uint8_t *data, size_t size, uint8_t **out_pixels, uint32_t *out_width, uint32_t *out_height, uint32_t *out_stride)
{
    int width;
    int height;
    int channels;
    uint8_t *rgba;
    uint8_t *bgra;
    uint32_t row;
    uint32_t stride;

    *out_pixels = NULL;
    *out_width = 0;
    *out_height = 0;
    *out_stride = 0;

    if (data == NULL || size == 0 || size > (size_t)INT_MAX) {
        return -1;
    }

    rgba = stbi_load_from_memory(data, (int)size, &width, &height, &channels, 4);
    if (rgba == NULL || width <= 0 || height <= 0) {
        stbi_image_free(rgba);
        return -1;
    }

    if ((uint32_t)width > UINT32_MAX / 4u || (uint32_t)height > UINT32_MAX / ((uint32_t)width * 4u)) {
        stbi_image_free(rgba);
        return -1;
    }

    stride = (uint32_t)width * 4u;
    bgra = (uint8_t *)malloc((size_t)stride * (size_t)height);
    if (bgra == NULL) {
        stbi_image_free(rgba);
        return -1;
    }

    for (row = 0; row < (uint32_t)height; ++row) {
        uint32_t column;
        const uint8_t *source_row = rgba + (size_t)row * (size_t)width * 4u;
        uint8_t *destination_row = bgra + (size_t)row * stride;

        for (column = 0; column < (uint32_t)width; ++column) {
            destination_row[column * 4u + 0u] = source_row[column * 4u + 2u];
            destination_row[column * 4u + 1u] = source_row[column * 4u + 1u];
            destination_row[column * 4u + 2u] = source_row[column * 4u + 0u];
            destination_row[column * 4u + 3u] = 255u;
        }
    }

    stbi_image_free(rgba);
    *out_pixels = bgra;
    *out_width = (uint32_t)width;
    *out_height = (uint32_t)height;
    *out_stride = stride;
    return 0;
}

int ss_jpeg_encode_bgra(const uint8_t *bgra, uint32_t width, uint32_t height, uint32_t stride, float quality, uint8_t **out_data, size_t *out_size)
{
    if (ss_jpeg_backend == SS_JPEG_BACKEND_C) {
        return ss_jpeg_c_encode_bgra(bgra, width, height, stride, quality, out_data, out_size);
    }

    return ss_jpeg_windows_encode_bgra(bgra, width, height, stride, quality, out_data, out_size);
}

int ss_jpeg_decode_to_bgra(const uint8_t *data, size_t size, uint8_t **out_pixels, uint32_t *out_width, uint32_t *out_height, uint32_t *out_stride)
{
    if (ss_jpeg_backend == SS_JPEG_BACKEND_C) {
        return ss_jpeg_c_decode_to_bgra(data, size, out_pixels, out_width, out_height, out_stride);
    }

    return ss_jpeg_windows_decode_to_bgra(data, size, out_pixels, out_width, out_height, out_stride);
}

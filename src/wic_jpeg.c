#define WIN32_LEAN_AND_MEAN

#include "wic_jpeg.h"

#include <windows.h>
#include <wincodec.h>
#include <ole2.h>
#include <objbase.h>
#include <stdlib.h>

static IWICImagingFactory *ss_wic_factory = NULL;

static IWICImagingFactory *ss_jpeg_get_factory(void)
{
    if (ss_wic_factory == NULL) {
        if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&ss_wic_factory))) {
            return NULL;
        }
    }

    return ss_wic_factory;
}

void ss_jpeg_shutdown(void)
{
    if (ss_wic_factory != NULL) {
        ss_wic_factory->lpVtbl->Release(ss_wic_factory);
        ss_wic_factory = NULL;
    }
}

static int ss_copy_stream_to_buffer(IStream *stream, uint8_t **out_data, size_t *out_size)
{
    STATSTG statistics;
    HGLOBAL global_handle;
    void *locked_data = NULL;
    SIZE_T size;
    uint8_t *buffer;

    if (stream->lpVtbl->Stat(stream, &statistics, STATFLAG_NONAME) != S_OK) {
        return -1;
    }

    if (statistics.cbSize.QuadPart == 0) {
        return -1;
    }

    if (statistics.cbSize.QuadPart > (ULONGLONG)(SIZE_MAX)) {
        return -1;
    }

    size = (SIZE_T)statistics.cbSize.QuadPart;

    if (GetHGlobalFromStream(stream, &global_handle) != S_OK) {
        return -1;
    }

    locked_data = GlobalLock(global_handle);
    if (locked_data == NULL) {
        return -1;
    }

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

int ss_jpeg_encode_bgra(const uint8_t *bgra, uint32_t width, uint32_t height, uint32_t stride, float quality, uint8_t **out_data, size_t *out_size)
{
    IWICImagingFactory *factory = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IPropertyBag2 *options = NULL;
    IStream *stream = NULL;
    PROPBAG2 property;
    VARIANT value;
    HRESULT hr;

    *out_data = NULL;
    *out_size = 0;

    factory = ss_jpeg_get_factory();
    if (factory == NULL) {
        return -1;
    }

    hr = CreateStreamOnHGlobal(NULL, TRUE, &stream);
    if (FAILED(hr)) {
        return -1;
    }

    hr = factory->lpVtbl->CreateEncoder(factory, &GUID_ContainerFormatJpeg, NULL, &encoder);
    if (FAILED(hr)) {
        stream->lpVtbl->Release(stream);
        return -1;
    }

    hr = encoder->lpVtbl->Initialize(encoder, stream, WICBitmapEncoderNoCache);
    if (FAILED(hr)) {
        encoder->lpVtbl->Release(encoder);
        stream->lpVtbl->Release(stream);
        return -1;
    }

    hr = encoder->lpVtbl->CreateNewFrame(encoder, &frame, &options);
    if (FAILED(hr)) {
        encoder->lpVtbl->Release(encoder);
        stream->lpVtbl->Release(stream);
        return -1;
    }

    ZeroMemory(&property, sizeof(property));
    property.pstrName = L"ImageQuality";
    VariantInit(&value);
    value.vt = VT_R4;
    value.fltVal = quality;
    options->lpVtbl->Write(options, 1, &property, &value);

    hr = frame->lpVtbl->Initialize(frame, options);
    if (FAILED(hr)) {
        options->lpVtbl->Release(options);
        frame->lpVtbl->Release(frame);
        encoder->lpVtbl->Release(encoder);
        stream->lpVtbl->Release(stream);
        return -1;
    }

    hr = frame->lpVtbl->SetSize(frame, width, height);
    if (FAILED(hr)) {
        options->lpVtbl->Release(options);
        frame->lpVtbl->Release(frame);
        encoder->lpVtbl->Release(encoder);
        stream->lpVtbl->Release(stream);
        return -1;
    }

    {
        WICPixelFormatGUID pixel_format = GUID_WICPixelFormat32bppBGRA;
        uint8_t *converted = NULL;
        const uint8_t *write_pixels = bgra;
        uint32_t write_stride = stride;

        hr = frame->lpVtbl->SetPixelFormat(frame, &pixel_format);
        if (FAILED(hr)) {
            options->lpVtbl->Release(options);
            frame->lpVtbl->Release(frame);
            encoder->lpVtbl->Release(encoder);
            stream->lpVtbl->Release(stream);
            return -1;
        }

        /* JPEG has no alpha; the encoder commonly negotiates down to 24bpp BGR */
        if (!IsEqualGUID(&pixel_format, &GUID_WICPixelFormat32bppBGRA)) {
            if (!IsEqualGUID(&pixel_format, &GUID_WICPixelFormat24bppBGR)) {
                options->lpVtbl->Release(options);
                frame->lpVtbl->Release(frame);
                encoder->lpVtbl->Release(encoder);
                stream->lpVtbl->Release(stream);
                return -1;
            }

            write_stride = width * 3u;
            converted = (uint8_t *)malloc((size_t)write_stride * height);
            if (converted == NULL) {
                options->lpVtbl->Release(options);
                frame->lpVtbl->Release(frame);
                encoder->lpVtbl->Release(encoder);
                stream->lpVtbl->Release(stream);
                return -1;
            }

            {
                uint32_t row;
                for (row = 0; row < height; ++row) {
                    uint32_t col;
                    const uint8_t *source_row = bgra + (size_t)row * stride;
                    uint8_t *destination_row = converted + (size_t)row * write_stride;
                    for (col = 0; col < width; ++col) {
                        destination_row[col * 3 + 0] = source_row[col * 4 + 0];
                        destination_row[col * 3 + 1] = source_row[col * 4 + 1];
                        destination_row[col * 3 + 2] = source_row[col * 4 + 2];
                    }
                }
            }

            write_pixels = converted;
        }

        hr = frame->lpVtbl->WritePixels(frame, height, write_stride, write_stride * height, (BYTE *)write_pixels);
        free(converted);
    }

    if (FAILED(hr)) {
        options->lpVtbl->Release(options);
        frame->lpVtbl->Release(frame);
        encoder->lpVtbl->Release(encoder);
        stream->lpVtbl->Release(stream);
        return -1;
    }

    hr = frame->lpVtbl->Commit(frame);
    if (FAILED(hr)) {
        options->lpVtbl->Release(options);
        frame->lpVtbl->Release(frame);
        encoder->lpVtbl->Release(encoder);
        stream->lpVtbl->Release(stream);
        return -1;
    }

    hr = encoder->lpVtbl->Commit(encoder);
    if (FAILED(hr)) {
        options->lpVtbl->Release(options);
        frame->lpVtbl->Release(frame);
        encoder->lpVtbl->Release(encoder);
        stream->lpVtbl->Release(stream);
        return -1;
    }

    options->lpVtbl->Release(options);
    frame->lpVtbl->Release(frame);
    encoder->lpVtbl->Release(encoder);

    if (ss_copy_stream_to_buffer(stream, out_data, out_size) != 0) {
        stream->lpVtbl->Release(stream);
        return -1;
    }

    stream->lpVtbl->Release(stream);
    return 0;
}

int ss_jpeg_decode_to_bgra(const uint8_t *data, size_t size, uint8_t **out_pixels, uint32_t *out_width, uint32_t *out_height, uint32_t *out_stride)
{
    IWICImagingFactory *factory = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *converter = NULL;
    IStream *memory_stream = NULL;
    UINT width;
    UINT height;
    uint32_t stride;
    uint8_t *pixels;
    HRESULT hr;

    *out_pixels = NULL;
    *out_width = 0;
    *out_height = 0;
    *out_stride = 0;

    factory = ss_jpeg_get_factory();
    if (factory == NULL) {
        return -1;
    }

    hr = CreateStreamOnHGlobal(NULL, TRUE, &memory_stream);
    if (FAILED(hr)) {
        return -1;
    }

    hr = memory_stream->lpVtbl->Write(memory_stream, data, (ULONG)size, NULL);
    if (FAILED(hr)) {
        memory_stream->lpVtbl->Release(memory_stream);
        return -1;
    }

    memory_stream->lpVtbl->Seek(memory_stream, (LARGE_INTEGER){0}, STREAM_SEEK_SET, NULL);

    hr = factory->lpVtbl->CreateDecoderFromStream(factory, memory_stream, NULL, WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr)) {
        memory_stream->lpVtbl->Release(memory_stream);
        return -1;
    }

    hr = decoder->lpVtbl->GetFrame(decoder, 0, &frame);
    if (FAILED(hr)) {
        decoder->lpVtbl->Release(decoder);
        memory_stream->lpVtbl->Release(memory_stream);
        return -1;
    }

    hr = factory->lpVtbl->CreateFormatConverter(factory, &converter);
    if (FAILED(hr)) {
        frame->lpVtbl->Release(frame);
        decoder->lpVtbl->Release(decoder);
        memory_stream->lpVtbl->Release(memory_stream);
        return -1;
    }

    hr = converter->lpVtbl->Initialize(converter, (IWICBitmapSource *)frame, &GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        converter->lpVtbl->Release(converter);
        frame->lpVtbl->Release(frame);
        decoder->lpVtbl->Release(decoder);
        memory_stream->lpVtbl->Release(memory_stream);
        return -1;
    }

    hr = converter->lpVtbl->GetSize(converter, &width, &height);
    if (FAILED(hr)) {
        converter->lpVtbl->Release(converter);
        frame->lpVtbl->Release(frame);
        decoder->lpVtbl->Release(decoder);
        memory_stream->lpVtbl->Release(memory_stream);
        return -1;
    }

    stride = width * 4u;
    pixels = (uint8_t *)malloc((size_t)stride * (size_t)height);
    if (pixels == NULL) {
        converter->lpVtbl->Release(converter);
        frame->lpVtbl->Release(frame);
        decoder->lpVtbl->Release(decoder);
        memory_stream->lpVtbl->Release(memory_stream);
        return -1;
    }

    hr = converter->lpVtbl->CopyPixels(converter, NULL, stride, stride * height, pixels);
    if (FAILED(hr)) {
        free(pixels);
        converter->lpVtbl->Release(converter);
        frame->lpVtbl->Release(frame);
        decoder->lpVtbl->Release(decoder);
        memory_stream->lpVtbl->Release(memory_stream);
        return -1;
    }

    *out_pixels = pixels;
    *out_width = width;
    *out_height = height;
    *out_stride = stride;

    converter->lpVtbl->Release(converter);
    frame->lpVtbl->Release(frame);
    decoder->lpVtbl->Release(decoder);
    memory_stream->lpVtbl->Release(memory_stream);
    return 0;
}

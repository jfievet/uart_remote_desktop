#define WIN32_LEAN_AND_MEAN
#define COBJMACROS

#include "capture_dxgi.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdlib.h>
#include <string.h>

/* only the capture thread ever calls into this module, so module-static state needs no locking */
typedef struct ss_dxgi_state {
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    IDXGIOutputDuplication *duplication;
    ID3D11Texture2D *staging_texture;
    uint8_t *last_frame_pixels; /* served back on AcquireNextFrame timeout, i.e. nothing changed */
    UINT width;
    UINT height;
    int initialized;
} ss_dxgi_state_t;

static ss_dxgi_state_t g_state;

static void ss_dxgi_release_duplication(void)
{
    if (g_state.duplication != NULL) {
        IDXGIOutputDuplication_Release(g_state.duplication);
        g_state.duplication = NULL;
    }
}

static void ss_dxgi_release_staging(void)
{
    if (g_state.staging_texture != NULL) {
        ID3D11Texture2D_Release(g_state.staging_texture);
        g_state.staging_texture = NULL;
    }
}

static void ss_dxgi_release_device(void)
{
    ss_dxgi_release_staging();
    ss_dxgi_release_duplication();

    if (g_state.context != NULL) {
        ID3D11DeviceContext_Release(g_state.context);
        g_state.context = NULL;
    }
    if (g_state.device != NULL) {
        ID3D11Device_Release(g_state.device);
        g_state.device = NULL;
    }

    g_state.width = 0;
    g_state.height = 0;
    g_state.initialized = 0;
}

static int ss_dxgi_init(void)
{
    HRESULT hr;
    IDXGIDevice *dxgi_device = NULL;
    IDXGIAdapter *adapter = NULL;
    IDXGIOutput *output = NULL;
    IDXGIOutput1 *output1 = NULL;
    D3D_FEATURE_LEVEL feature_level;

    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &g_state.device, &feature_level, &g_state.context);
    if (FAILED(hr) || g_state.device == NULL) {
        return -1;
    }

    hr = ID3D11Device_QueryInterface(g_state.device, &IID_IDXGIDevice, (void **)&dxgi_device);
    if (FAILED(hr) || dxgi_device == NULL) {
        ss_dxgi_release_device();
        return -1;
    }

    hr = IDXGIDevice_GetParent(dxgi_device, &IID_IDXGIAdapter, (void **)&adapter);
    IDXGIDevice_Release(dxgi_device);
    if (FAILED(hr) || adapter == NULL) {
        ss_dxgi_release_device();
        return -1;
    }

    hr = IDXGIAdapter_EnumOutputs(adapter, 0, &output);
    IDXGIAdapter_Release(adapter);
    if (FAILED(hr) || output == NULL) {
        ss_dxgi_release_device();
        return -1;
    }

    hr = IDXGIOutput_QueryInterface(output, &IID_IDXGIOutput1, (void **)&output1);
    IDXGIOutput_Release(output);
    if (FAILED(hr) || output1 == NULL) {
        ss_dxgi_release_device();
        return -1;
    }

    hr = IDXGIOutput1_DuplicateOutput(output1, (IUnknown *)g_state.device, &g_state.duplication);
    IDXGIOutput1_Release(output1);
    if (FAILED(hr) || g_state.duplication == NULL) {
        ss_dxgi_release_device();
        return -1;
    }

    g_state.initialized = 1;
    return 0;
}

static int ss_dxgi_ensure_staging_texture(const D3D11_TEXTURE2D_DESC *source_desc)
{
    D3D11_TEXTURE2D_DESC staging_desc;
    HRESULT hr;

    if (g_state.staging_texture != NULL && g_state.width == source_desc->Width && g_state.height == source_desc->Height) {
        return 0;
    }

    ss_dxgi_release_staging();
    free(g_state.last_frame_pixels);
    g_state.last_frame_pixels = NULL;

    ZeroMemory(&staging_desc, sizeof(staging_desc));
    staging_desc.Width = source_desc->Width;
    staging_desc.Height = source_desc->Height;
    staging_desc.MipLevels = 1;
    staging_desc.ArraySize = 1;
    staging_desc.Format = source_desc->Format;
    staging_desc.SampleDesc.Count = 1;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    hr = ID3D11Device_CreateTexture2D(g_state.device, &staging_desc, NULL, &g_state.staging_texture);
    if (FAILED(hr) || g_state.staging_texture == NULL) {
        return -1;
    }

    g_state.width = source_desc->Width;
    g_state.height = source_desc->Height;
    return 0;
}

static int ss_dxgi_serve_last_frame(ss_frame_t *frame)
{
    size_t buffer_size;

    if (g_state.last_frame_pixels == NULL) {
        return -1;
    }

    buffer_size = (size_t)g_state.width * (size_t)g_state.height * 4u;
    frame->pixels = (uint8_t *)malloc(buffer_size);
    if (frame->pixels == NULL) {
        return -1;
    }

    memcpy(frame->pixels, g_state.last_frame_pixels, buffer_size);
    frame->width = g_state.width;
    frame->height = g_state.height;
    frame->stride = g_state.width * 4u;
    return 0;
}

int ss_capture_dxgi_capture(ss_frame_t *frame)
{
    HRESULT hr;
    IDXGIResource *desktop_resource = NULL;
    ID3D11Texture2D *acquired_texture = NULL;
    DXGI_OUTDUPL_FRAME_INFO frame_info;
    D3D11_TEXTURE2D_DESC desc;
    D3D11_MAPPED_SUBRESOURCE mapped;
    size_t buffer_size;
    UINT row;

    if (!g_state.initialized) {
        if (ss_dxgi_init() != 0) {
            return -1;
        }
    }

    /* poll rather than block -- an idle desktop would otherwise stall each call for the full timeout */
    hr = IDXGIOutputDuplication_AcquireNextFrame(g_state.duplication, 0, &frame_info, &desktop_resource);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        /* nothing changed on screen since the last capture -- reuse it rather than treat this as an error */
        return ss_dxgi_serve_last_frame(frame);
    }

    if (hr == DXGI_ERROR_ACCESS_LOST) {
        /* lock screen / GPU reset / mode change -- reinitialize the duplication on the next call */
        ss_dxgi_release_duplication();
        g_state.initialized = 0;
        return -1;
    }

    if (FAILED(hr) || desktop_resource == NULL) {
        return -1;
    }

    hr = IDXGIResource_QueryInterface(desktop_resource, &IID_ID3D11Texture2D, (void **)&acquired_texture);
    IDXGIResource_Release(desktop_resource);
    if (FAILED(hr) || acquired_texture == NULL) {
        IDXGIOutputDuplication_ReleaseFrame(g_state.duplication);
        return -1;
    }

    ID3D11Texture2D_GetDesc(acquired_texture, &desc);

    if (ss_dxgi_ensure_staging_texture(&desc) != 0) {
        ID3D11Texture2D_Release(acquired_texture);
        IDXGIOutputDuplication_ReleaseFrame(g_state.duplication);
        return -1;
    }

    ID3D11DeviceContext_CopyResource(g_state.context, (ID3D11Resource *)g_state.staging_texture, (ID3D11Resource *)acquired_texture);
    ID3D11Texture2D_Release(acquired_texture);

    hr = ID3D11DeviceContext_Map(g_state.context, (ID3D11Resource *)g_state.staging_texture, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        IDXGIOutputDuplication_ReleaseFrame(g_state.duplication);
        return -1;
    }

    buffer_size = (size_t)g_state.width * (size_t)g_state.height * 4u;
    frame->pixels = (uint8_t *)malloc(buffer_size);
    if (frame->pixels == NULL) {
        ID3D11DeviceContext_Unmap(g_state.context, (ID3D11Resource *)g_state.staging_texture, 0);
        IDXGIOutputDuplication_ReleaseFrame(g_state.duplication);
        return -1;
    }

    for (row = 0; row < g_state.height; ++row) {
        const uint8_t *source_row = (const uint8_t *)mapped.pData + (size_t)row * mapped.RowPitch;
        uint8_t *destination_row = frame->pixels + (size_t)row * g_state.width * 4u;
        memcpy(destination_row, source_row, (size_t)g_state.width * 4u);
    }

    ID3D11DeviceContext_Unmap(g_state.context, (ID3D11Resource *)g_state.staging_texture, 0);
    IDXGIOutputDuplication_ReleaseFrame(g_state.duplication);

    frame->width = g_state.width;
    frame->height = g_state.height;
    frame->stride = g_state.width * 4u;

    free(g_state.last_frame_pixels);
    g_state.last_frame_pixels = (uint8_t *)malloc(buffer_size);
    if (g_state.last_frame_pixels != NULL) {
        memcpy(g_state.last_frame_pixels, frame->pixels, buffer_size);
    }

    return 0;
}

void ss_capture_dxgi_shutdown(void)
{
    free(g_state.last_frame_pixels);
    g_state.last_frame_pixels = NULL;
    ss_dxgi_release_device();
}

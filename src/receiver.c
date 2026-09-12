#define WIN32_LEAN_AND_MEAN

#include "protocol.h"
#include "transport.h"
#include "wic_jpeg.h"
#include "win_compat.h"

#include <windows.h>
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SS_WINDOW_CLASS_NAME "ScreenSharingReceiverWindow"
#define SS_APP_FRAME_READY (WM_APP + 1)
#define SS_MOUSE_MOVE_MIN_INTERVAL_MS 15u

typedef struct receiver_framebuffer {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint8_t *pixels;
} receiver_framebuffer_t;

typedef struct receiver_config {
    unsigned short port;
    int ber_mode;
    ss_transport_type_t transport_type;
    unsigned int com_port;
    unsigned long baud_rate;
    ss_jpeg_backend_t jpeg_backend;
} receiver_config_t;

typedef struct receiver_context {
    ss_transport_t transport;
    HANDLE stop_event;
    HWND window_handle;
    HANDLE worker_thread;
    CRITICAL_SECTION framebuffer_lock;
    receiver_framebuffer_t framebuffer;
    receiver_config_t config;
    uint64_t last_mouse_move_tick;
} receiver_context_t;

static void receiver_framebuffer_release(receiver_framebuffer_t *framebuffer)
{
    free(framebuffer->pixels);
    framebuffer->pixels = NULL;
    framebuffer->width = 0;
    framebuffer->height = 0;
    framebuffer->stride = 0;
}

static void receiver_framebuffer_replace(receiver_framebuffer_t *framebuffer, const uint8_t *pixels, uint32_t width, uint32_t height, uint32_t stride)
{
    uint8_t *copy = (uint8_t *)malloc((size_t)stride * (size_t)height);
    if (copy == NULL) {
        return;
    }

    memcpy(copy, pixels, (size_t)stride * (size_t)height);
    receiver_framebuffer_release(framebuffer);
    framebuffer->pixels = copy;
    framebuffer->width = width;
    framebuffer->height = height;
    framebuffer->stride = stride;
}

static void receiver_framebuffer_patch(receiver_framebuffer_t *framebuffer, uint32_t x, uint32_t y, uint32_t width, uint32_t height, const uint8_t *pixels, uint32_t stride)
{
    uint32_t row;

    if (framebuffer->pixels == NULL) {
        return;
    }

    if (x + width > framebuffer->width || y + height > framebuffer->height) {
        return;
    }

    for (row = 0; row < height; ++row) {
        uint8_t *destination_row = framebuffer->pixels + (size_t)(y + row) * framebuffer->stride + (size_t)x * 4u;
        const uint8_t *source_row = pixels + (size_t)row * stride;
        memcpy(destination_row, source_row, (size_t)width * 4u);
    }
}

static void receiver_print_usage(void)
{
    printf("Usage: receiver [--tcp | --uart] [--port <port>] [--com <n>] [--speed <baud>] [--jpeg-backend windows|c] [--ber]\n");
}

static int receiver_parse_args(int argc, char **argv, receiver_config_t *config)
{
    int index;

    config->port = 5000;
    config->ber_mode = 0;
    config->transport_type = SS_TRANSPORT_TCP;
    config->com_port = 0;
    config->baud_rate = 3000000UL;
    config->jpeg_backend = SS_JPEG_BACKEND_WINDOWS;

    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--help") == 0 || strcmp(argv[index], "-h") == 0) {
            return 1;
        } else if (strcmp(argv[index], "--port") == 0 && index + 1 < argc) {
            config->port = (unsigned short)atoi(argv[++index]);
        } else if (strcmp(argv[index], "--ber") == 0) {
            config->ber_mode = 1;
        } else if (strcmp(argv[index], "--tcp") == 0) {
            config->transport_type = SS_TRANSPORT_TCP;
        } else if (strcmp(argv[index], "--uart") == 0) {
            config->transport_type = SS_TRANSPORT_UART;
        } else if (strcmp(argv[index], "--com") == 0 && index + 1 < argc) {
            config->com_port = (unsigned int)atoi(argv[++index]);
        } else if (strcmp(argv[index], "--speed") == 0 && index + 1 < argc) {
            config->baud_rate = (unsigned long)strtod(argv[++index], NULL);
        } else if (strcmp(argv[index], "--jpeg-backend") == 0 && index + 1 < argc) {
            if (ss_jpeg_parse_backend(argv[++index], &config->jpeg_backend) != 0) {
                return -1;
            }
        } else {
            return -1;
        }
    }

    if (config->port == 0) {
        return -1;
    }

    if (config->transport_type == SS_TRANSPORT_UART && config->com_port == 0) {
        return -1;
    }

    if (config->baud_rate == 0) {
        return -1;
    }

    return 0;
}

static int receiver_open_transport(ss_transport_t *transport, const receiver_config_t *config)
{
    if (config->transport_type == SS_TRANSPORT_UART) {
        return ss_uart_transport_open(transport, config->com_port, config->baud_rate);
    }

    return ss_tcp_transport_accept(transport, config->port);
}

#define SS_BER_CHUNK_SIZE 4096u
#define SS_BER_WINDOW_SECONDS 5u

static void receiver_run_ber_mode(const receiver_config_t *config)
{
    ss_transport_t transport;
    uint8_t buffer[SS_BER_CHUNK_SIZE];
    uint64_t window_bytes[SS_BER_WINDOW_SECONDS];
    int window_pos = 0;
    uint64_t last_print_tick;

    ZeroMemory(&transport, sizeof(transport));
    ZeroMemory(window_bytes, sizeof(window_bytes));

    printf("BER loopback mode: waiting for a connection...\n");

    if (receiver_open_transport(&transport, config) != 0) {
        printf("Unable to open the transport.\n");
        return;
    }

    printf("BER loopback mode: connected, relaying data back to the sender...\n");
    last_print_tick = ss_win_get_tick_count64();

    for (;;) {
        uint64_t now;

        if (ss_transport_recv_exact(&transport, buffer, sizeof(buffer)) != 0) {
            break;
        }

        if (ss_transport_send_all(&transport, buffer, sizeof(buffer)) != 0) {
            break;
        }

        window_bytes[window_pos] += sizeof(buffer);

        now = ss_win_get_tick_count64();
        if (now - last_print_tick >= 1000u) {
            uint64_t window_total = 0;
            int i;

            window_pos = (window_pos + 1) % (int)SS_BER_WINDOW_SECONDS;
            window_bytes[window_pos] = 0;

            for (i = 0; i < (int)SS_BER_WINDOW_SECONDS; ++i) {
                window_total += window_bytes[i];
            }

            printf("Throughput passing through : %.2f bps (avg over %us)\n",
                ((double)window_total * 8.0) / (double)SS_BER_WINDOW_SECONDS,
                SS_BER_WINDOW_SECONDS);
            fflush(stdout);
            last_print_tick = now;
        }
    }

    printf("BER loopback mode: connection closed.\n");
    ss_transport_close(&transport);
}

static void receiver_draw_frame(HWND window_handle, const receiver_context_t *context, HDC device_context)
{
    BITMAPINFO bitmap_info;
    RECT client_rect;
    int client_width;
    int client_height;
    int stretch_mode;

    ZeroMemory(&bitmap_info, sizeof(bitmap_info));
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;

    EnterCriticalSection((CRITICAL_SECTION *)&context->framebuffer_lock);
    if (context->framebuffer.pixels != NULL) {
        bitmap_info.bmiHeader.biWidth = (LONG)context->framebuffer.width;
        bitmap_info.bmiHeader.biHeight = -(LONG)context->framebuffer.height;
        GetClientRect(window_handle, &client_rect);
        client_width = client_rect.right - client_rect.left;
        client_height = client_rect.bottom - client_rect.top;
        stretch_mode = (client_width == (int)context->framebuffer.width && client_height == (int)context->framebuffer.height)
            ? COLORONCOLOR
            : HALFTONE;

        SetStretchBltMode(device_context, stretch_mode);
        if (stretch_mode == HALFTONE) {
            SetBrushOrgEx(device_context, 0, 0, NULL);
        }

        /* StretchDIBits does not reliably trigger the OS cursor exclusion/redraw like BitBlt does, so hide/show around it explicitly */
        ShowCursor(FALSE);
        StretchDIBits(
            device_context,
            0,
            0,
            client_width,
            client_height,
            0,
            0,
            (int)context->framebuffer.width,
            (int)context->framebuffer.height,
            context->framebuffer.pixels,
            &bitmap_info,
            DIB_RGB_COLORS,
            SRCCOPY);
        ShowCursor(TRUE);
    }

    LeaveCriticalSection((CRITICAL_SECTION *)&context->framebuffer_lock);
}

static void receiver_forward_mouse_event(receiver_context_t *context, HWND window_handle, int client_x, int client_y, ss_mouse_action_t action, int32_t param, int throttle_move)
{
    RECT client_rect;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint32_t remote_x;
    uint32_t remote_y;
    int client_width;
    int client_height;
    ss_message_header_t header;
    ss_mouse_event_t payload;

    if (context == NULL) {
        return;
    }

    EnterCriticalSection(&context->framebuffer_lock);
    framebuffer_width = context->framebuffer.width;
    framebuffer_height = context->framebuffer.height;
    LeaveCriticalSection(&context->framebuffer_lock);

    if (framebuffer_width == 0 || framebuffer_height == 0) {
        return;
    }

    GetClientRect(window_handle, &client_rect);
    client_width = client_rect.right - client_rect.left;
    client_height = client_rect.bottom - client_rect.top;
    if (client_width <= 0 || client_height <= 0) {
        return;
    }

    if (client_x < 0) {
        client_x = 0;
    } else if (client_x >= client_width) {
        client_x = client_width - 1;
    }

    if (client_y < 0) {
        client_y = 0;
    } else if (client_y >= client_height) {
        client_y = client_height - 1;
    }

    remote_x = (uint32_t)((int64_t)client_x * framebuffer_width / client_width);
    remote_y = (uint32_t)((int64_t)client_y * framebuffer_height / client_height);

    if (throttle_move) {
        uint64_t now = ss_win_get_tick_count64();
        if (now - context->last_mouse_move_tick < SS_MOUSE_MOVE_MIN_INTERVAL_MS) {
            return;
        }
        context->last_mouse_move_tick = now;
    }

    ss_protocol_make_header(&header, SS_MESSAGE_MOUSE_EVENT, 0, 0, 0, 0, sizeof(payload));
    ss_protocol_encode_mouse_event(&payload, remote_x, remote_y, action, param);

    if (ss_transport_send_all(&context->transport, &header, sizeof(header)) != 0) {
        return;
    }
    ss_transport_send_all(&context->transport, &payload, sizeof(payload));
}

static void receiver_forward_key_event(receiver_context_t *context, uint32_t vk_code, uint32_t scan_code, ss_key_action_t action, uint32_t extended)
{
    ss_message_header_t header;
    ss_key_event_t payload;

    if (context == NULL) {
        return;
    }

    ss_protocol_make_header(&header, SS_MESSAGE_KEY_EVENT, 0, 0, 0, 0, sizeof(payload));
    ss_protocol_encode_key_event(&payload, vk_code, scan_code, action, extended);

    if (ss_transport_send_all(&context->transport, &header, sizeof(header)) != 0) {
        return;
    }
    ss_transport_send_all(&context->transport, &payload, sizeof(payload));
}

static LRESULT CALLBACK receiver_window_proc(HWND window_handle, UINT message, WPARAM wparam, LPARAM lparam)
{
    receiver_context_t *context = (receiver_context_t *)GetWindowLongPtr(window_handle, GWLP_USERDATA);

    switch (message) {
    case WM_CREATE:
        SetWindowLongPtr(window_handle, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCT *)lparam)->lpCreateParams);
        return 0;

    case SS_APP_FRAME_READY:
        InvalidateRect(window_handle, NULL, FALSE);
        return 0;

    case WM_SIZE:
        InvalidateRect(window_handle, NULL, FALSE);
        return 0;

    case WM_PAINT:
    {
        PAINTSTRUCT paint_struct;
        HDC device_context = BeginPaint(window_handle, &paint_struct);
        if (context != NULL) {
            receiver_draw_frame(window_handle, context, device_context);
        }
        EndPaint(window_handle, &paint_struct);
        return 0;
    }

    case WM_MOUSEMOVE:
        receiver_forward_mouse_event(context, window_handle, (int)(short)LOWORD(lparam), (int)(short)HIWORD(lparam), SS_MOUSE_ACTION_MOVE, 0, 1);
        return 0;

    case WM_LBUTTONDOWN:
        SetCapture(window_handle);
        receiver_forward_mouse_event(context, window_handle, (int)(short)LOWORD(lparam), (int)(short)HIWORD(lparam), SS_MOUSE_ACTION_BUTTON_DOWN, SS_MOUSE_BUTTON_LEFT, 0);
        return 0;

    case WM_LBUTTONUP:
        receiver_forward_mouse_event(context, window_handle, (int)(short)LOWORD(lparam), (int)(short)HIWORD(lparam), SS_MOUSE_ACTION_BUTTON_UP, SS_MOUSE_BUTTON_LEFT, 0);
        ReleaseCapture();
        return 0;

    case WM_RBUTTONDOWN:
        receiver_forward_mouse_event(context, window_handle, (int)(short)LOWORD(lparam), (int)(short)HIWORD(lparam), SS_MOUSE_ACTION_BUTTON_DOWN, SS_MOUSE_BUTTON_RIGHT, 0);
        return 0;

    case WM_RBUTTONUP:
        receiver_forward_mouse_event(context, window_handle, (int)(short)LOWORD(lparam), (int)(short)HIWORD(lparam), SS_MOUSE_ACTION_BUTTON_UP, SS_MOUSE_BUTTON_RIGHT, 0);
        return 0;

    case WM_MBUTTONDOWN:
        receiver_forward_mouse_event(context, window_handle, (int)(short)LOWORD(lparam), (int)(short)HIWORD(lparam), SS_MOUSE_ACTION_BUTTON_DOWN, SS_MOUSE_BUTTON_MIDDLE, 0);
        return 0;

    case WM_MBUTTONUP:
        receiver_forward_mouse_event(context, window_handle, (int)(short)LOWORD(lparam), (int)(short)HIWORD(lparam), SS_MOUSE_ACTION_BUTTON_UP, SS_MOUSE_BUTTON_MIDDLE, 0);
        return 0;

    case WM_MOUSEWHEEL:
    {
        POINT screen_point;
        screen_point.x = (int)(short)LOWORD(lparam);
        screen_point.y = (int)(short)HIWORD(lparam);
        ScreenToClient(window_handle, &screen_point);
        receiver_forward_mouse_event(context, window_handle, screen_point.x, screen_point.y, SS_MOUSE_ACTION_WHEEL, (int32_t)((short)HIWORD(wparam)) / WHEEL_DELTA, 0);
        return 0;
    }

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        receiver_forward_key_event(context, (uint32_t)wparam, (uint32_t)((lparam >> 16) & 0xFFu), SS_KEY_ACTION_DOWN, (uint32_t)((lparam >> 24) & 0x1u));
        break;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        receiver_forward_key_event(context, (uint32_t)wparam, (uint32_t)((lparam >> 16) & 0xFFu), SS_KEY_ACTION_UP, (uint32_t)((lparam >> 24) & 0x1u));
        break;

    case WM_DESTROY:
        if (context != NULL) {
            SetEvent(context->stop_event);
            ss_transport_close(&context->transport);
        }
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(window_handle, message, wparam, lparam);
}

static DWORD WINAPI receiver_worker_thread(LPVOID parameter)
{
    receiver_context_t *context = (receiver_context_t *)parameter;
    HRESULT com_result;
    uint8_t *jpeg_data = NULL;
    uint8_t *decoded_pixels = NULL;
    uint32_t decoded_width = 0;
    uint32_t decoded_height = 0;
    uint32_t decoded_stride = 0;

    com_result = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(com_result)) {
        goto cleanup;
    }

    if (receiver_open_transport(&context->transport, &context->config) != 0) {
        goto cleanup;
    }

    while (WaitForSingleObject(context->stop_event, 0) != WAIT_OBJECT_0) {
        ss_message_header_t header;

        jpeg_data = NULL;
        decoded_pixels = NULL;
        decoded_width = 0;
        decoded_height = 0;
        decoded_stride = 0;

        if (ss_transport_recv_exact(&context->transport, &header, sizeof(header)) != 0) {
            goto cleanup;
        }

        if (!ss_protocol_validate_header(&header)) {
            goto cleanup;
        }

        jpeg_data = (uint8_t *)malloc(ntohl(header.payload_size));
        if (jpeg_data == NULL) {
            goto cleanup;
        }

        if (ss_transport_recv_exact(&context->transport, jpeg_data, ntohl(header.payload_size)) != 0) {
            goto cleanup;
        }

        if (ss_jpeg_decode_to_bgra(jpeg_data, ntohl(header.payload_size), &decoded_pixels, &decoded_width, &decoded_height, &decoded_stride) != 0) {
            goto cleanup;
        }

        EnterCriticalSection(&context->framebuffer_lock);
        if (ntohs(header.type) == SS_MESSAGE_FULL_FRAME || context->framebuffer.pixels == NULL) {
            receiver_framebuffer_replace(&context->framebuffer, decoded_pixels, decoded_width, decoded_height, decoded_stride);
        } else {
            receiver_framebuffer_patch(
                &context->framebuffer,
                ntohl(header.x),
                ntohl(header.y),
                ntohl(header.width),
                ntohl(header.height),
                decoded_pixels,
                decoded_stride);
        }
        LeaveCriticalSection(&context->framebuffer_lock);

        free(decoded_pixels);
        decoded_pixels = NULL;
        free(jpeg_data);
        jpeg_data = NULL;
        PostMessage(context->window_handle, SS_APP_FRAME_READY, 0, 0);
    }

cleanup:
    free(decoded_pixels);
    free(jpeg_data);
    PostMessage(context->window_handle, WM_CLOSE, 0, 0);
    CoUninitialize();
    return 0;
}

int main(int argc, char **argv)
{
    WNDCLASS window_class;
    MSG message;
    receiver_context_t context;
    WSADATA wsa_data;
    receiver_config_t config;
    HWND window_handle;
    int exit_code = 0;

    ZeroMemory(&context, sizeof(context));

    ss_win_set_process_dpi_aware(); /* otherwise the window/client rect is DPI-scaled, not real pixels */

    {
        int parse_result = receiver_parse_args(argc, argv, &config);
        if (parse_result != 0) {
            receiver_print_usage();
            return parse_result > 0 ? 0 : 1;
        }
    }

    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        printf("WSAStartup failed.\n");
        return 1;
    }

    if (config.ber_mode) {
        receiver_run_ber_mode(&config);
        WSACleanup();
        return 0;
    }

    ss_jpeg_set_backend(config.jpeg_backend);

    {
        HRESULT com_result = CoInitializeEx(NULL, COINIT_MULTITHREADED);
        if (com_result != S_OK && com_result != S_FALSE) {
            printf("COM initialization failed.\n");
            WSACleanup();
            return 1;
        }
    }

    InitializeCriticalSection(&context.framebuffer_lock);
    context.stop_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    context.config = config;

    ZeroMemory(&window_class, sizeof(window_class));
    window_class.lpfnWndProc = receiver_window_proc;
    window_class.hInstance = GetModuleHandle(NULL);
    window_class.lpszClassName = SS_WINDOW_CLASS_NAME;
    window_class.hCursor = LoadCursor(NULL, IDC_ARROW);

    if (!RegisterClass(&window_class)) {
        printf("Unable to register window class.\n");
        DeleteCriticalSection(&context.framebuffer_lock);
        CloseHandle(context.stop_event);
        CoUninitialize();
        WSACleanup();
        return 1;
    }

    window_handle = CreateWindowEx(
        0,
        SS_WINDOW_CLASS_NAME,
        "Screen Sharing Receiver",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        960,
        540,
        NULL,
        NULL,
        GetModuleHandle(NULL),
        &context);

    if (window_handle == NULL) {
        printf("Unable to create window.\n");
        DeleteCriticalSection(&context.framebuffer_lock);
        CloseHandle(context.stop_event);
        CoUninitialize();
        WSACleanup();
        return 1;
    }

    context.window_handle = window_handle;
    context.worker_thread = CreateThread(NULL, 0, receiver_worker_thread, &context, 0, NULL);
    if (context.worker_thread == NULL) {
        printf("Unable to create worker thread.\n");
        DestroyWindow(window_handle);
        DeleteCriticalSection(&context.framebuffer_lock);
        CloseHandle(context.stop_event);
        CoUninitialize();
        WSACleanup();
        return 1;
    }

    ShowWindow(window_handle, SW_SHOWDEFAULT);
    UpdateWindow(window_handle);

    while (GetMessage(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessage(&message);
    }

    WaitForSingleObject(context.worker_thread, INFINITE);
    CloseHandle(context.worker_thread);
    receiver_framebuffer_release(&context.framebuffer);
    DeleteCriticalSection(&context.framebuffer_lock);
    CloseHandle(context.stop_event);
    ss_jpeg_shutdown();
    CoUninitialize();
    WSACleanup();
    return exit_code;
}

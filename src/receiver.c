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
    int debug_enabled;
} receiver_config_t;

typedef enum receiver_debug_stage {
    RECEIVER_DEBUG_STAGE_RECV_HEADER = 0,
    RECEIVER_DEBUG_STAGE_RECV_PAYLOAD,
    RECEIVER_DEBUG_STAGE_DECODE,
    RECEIVER_DEBUG_STAGE_FRAMEBUFFER,
    RECEIVER_DEBUG_STAGE_COUNT
} receiver_debug_stage_t;

typedef struct receiver_debug_stats {
    CRITICAL_SECTION lock;
    double stage_total_ms[RECEIVER_DEBUG_STAGE_COUNT];
    uint64_t stage_count[RECEIVER_DEBUG_STAGE_COUNT];
} receiver_debug_stats_t;

static const char *receiver_debug_stage_names[RECEIVER_DEBUG_STAGE_COUNT] = { "recv-header", "recv-payload", "decode", "framebuffer" };

static void receiver_debug_stats_init(receiver_debug_stats_t *stats)
{
    ZeroMemory(stats, sizeof(*stats));
    InitializeCriticalSection(&stats->lock);
}

static void receiver_debug_stats_destroy(receiver_debug_stats_t *stats)
{
    DeleteCriticalSection(&stats->lock);
}

static void receiver_debug_stats_add(receiver_debug_stats_t *stats, receiver_debug_stage_t stage, double elapsed_ms)
{
    if (stats == NULL) {
        return;
    }

    EnterCriticalSection(&stats->lock);
    stats->stage_total_ms[stage] += elapsed_ms;
    stats->stage_count[stage] += 1;
    LeaveCriticalSection(&stats->lock);
}

static void receiver_print_debug_stats(receiver_debug_stats_t *stats)
{
    int stage;

    EnterCriticalSection(&stats->lock);
    printf("Debug stage timings (avg ms/call)");
    for (stage = 0; stage < RECEIVER_DEBUG_STAGE_COUNT; ++stage) {
        double average_ms = stats->stage_count[stage] > 0 ? stats->stage_total_ms[stage] / (double)stats->stage_count[stage] : 0.0;
        printf(" | %s : %.3f ms (%llu calls)", receiver_debug_stage_names[stage], average_ms, (unsigned long long)stats->stage_count[stage]);
        stats->stage_total_ms[stage] = 0.0;
        stats->stage_count[stage] = 0;
    }
    printf("\n");
    fflush(stdout);
    LeaveCriticalSection(&stats->lock);
}

/* ---- decode job queue: I/O thread -> pool of decode worker threads ---- */

typedef struct receiver_decode_job {
    struct receiver_decode_job *next;
    uint8_t *jpeg_data;
    size_t jpeg_size;
    uint32_t x;
    uint32_t y;
} receiver_decode_job_t;

typedef struct receiver_decode_queue {
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE not_empty;
    receiver_decode_job_t *head;
    receiver_decode_job_t *tail;
    int shutdown;
} receiver_decode_queue_t;

static void receiver_decode_queue_init(receiver_decode_queue_t *queue)
{
    ZeroMemory(queue, sizeof(*queue));
    InitializeCriticalSection(&queue->lock);
    InitializeConditionVariable(&queue->not_empty);
}

static void receiver_decode_job_free(receiver_decode_job_t *job)
{
    if (job != NULL) {
        free(job->jpeg_data);
        free(job);
    }
}

static void receiver_decode_queue_destroy(receiver_decode_queue_t *queue)
{
    receiver_decode_job_t *job;

    EnterCriticalSection(&queue->lock);
    job = queue->head;
    queue->head = NULL;
    queue->tail = NULL;
    LeaveCriticalSection(&queue->lock);

    while (job != NULL) {
        receiver_decode_job_t *next = job->next;
        receiver_decode_job_free(job);
        job = next;
    }

    DeleteCriticalSection(&queue->lock);
}

static void receiver_decode_queue_push(receiver_decode_queue_t *queue, receiver_decode_job_t *job)
{
    job->next = NULL;

    EnterCriticalSection(&queue->lock);
    if (queue->tail == NULL) {
        queue->head = job;
        queue->tail = job;
    } else {
        queue->tail->next = job;
        queue->tail = job;
    }
    LeaveCriticalSection(&queue->lock);
    WakeConditionVariable(&queue->not_empty);
}

static void receiver_decode_queue_signal_shutdown(receiver_decode_queue_t *queue)
{
    EnterCriticalSection(&queue->lock);
    queue->shutdown = 1;
    LeaveCriticalSection(&queue->lock);
    WakeAllConditionVariable(&queue->not_empty);
}

static int receiver_decode_queue_is_shutdown(receiver_decode_queue_t *queue)
{
    int result;

    EnterCriticalSection(&queue->lock);
    result = queue->shutdown;
    LeaveCriticalSection(&queue->lock);
    return result;
}

static receiver_decode_job_t *receiver_decode_queue_pop_wait(receiver_decode_queue_t *queue, DWORD timeout_ms)
{
    receiver_decode_job_t *job = NULL;

    EnterCriticalSection(&queue->lock);
    if (queue->head == NULL && !queue->shutdown) {
        SleepConditionVariableCS(&queue->not_empty, &queue->lock, timeout_ms);
    }
    if (queue->head != NULL) {
        job = queue->head;
        queue->head = job->next;
        if (queue->head == NULL) {
            queue->tail = NULL;
        }
    }
    LeaveCriticalSection(&queue->lock);

    return job;
}

typedef struct receiver_context {
    ss_transport_t transport;
    HANDLE stop_event;
    HWND window_handle;
    HANDLE io_thread;
    CRITICAL_SECTION framebuffer_lock;
    receiver_framebuffer_t framebuffer;
    receiver_framebuffer_t render_snapshot; /* UI-thread-only; refreshed from framebuffer under a short lock before painting */
    receiver_decode_queue_t decode_queue;
    receiver_config_t config;
    uint64_t last_mouse_move_tick;
    receiver_debug_stats_t *debug_stats;
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
    printf("Usage: receiver [--tcp | --uart] [--port <port>] [--com <n>] [--speed <baud>] [--jpeg-backend windows|c] [--ber] [--debug]\n");
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
        } else if (strcmp(argv[index], "--debug") == 0) {
            config->debug_enabled = 1;
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

static void receiver_draw_frame(HWND window_handle, receiver_context_t *context, HDC device_context)
{
    BITMAPINFO bitmap_info;
    RECT client_rect;
    int client_width;
    int client_height;
    int stretch_mode;

    /* copy out under a short lock instead of holding framebuffer_lock for the whole StretchDIBits call below --
       that used to stall the decode worker(s) for the entire blit duration */
    EnterCriticalSection(&context->framebuffer_lock);
    if (context->framebuffer.pixels != NULL) {
        size_t size = (size_t)context->framebuffer.stride * (size_t)context->framebuffer.height;

        if (context->render_snapshot.pixels == NULL || context->render_snapshot.width != context->framebuffer.width || context->render_snapshot.height != context->framebuffer.height) {
            free(context->render_snapshot.pixels);
            context->render_snapshot.pixels = (uint8_t *)malloc(size);
            context->render_snapshot.width = context->framebuffer.width;
            context->render_snapshot.height = context->framebuffer.height;
            context->render_snapshot.stride = context->framebuffer.stride;
        }
        if (context->render_snapshot.pixels != NULL) {
            memcpy(context->render_snapshot.pixels, context->framebuffer.pixels, size);
        }
    }
    LeaveCriticalSection(&context->framebuffer_lock);

    if (context->render_snapshot.pixels == NULL) {
        return;
    }

    ZeroMemory(&bitmap_info, sizeof(bitmap_info));
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;
    bitmap_info.bmiHeader.biWidth = (LONG)context->render_snapshot.width;
    bitmap_info.bmiHeader.biHeight = -(LONG)context->render_snapshot.height;

    GetClientRect(window_handle, &client_rect);
    client_width = client_rect.right - client_rect.left;
    client_height = client_rect.bottom - client_rect.top;
    stretch_mode = (client_width == (int)context->render_snapshot.width && client_height == (int)context->render_snapshot.height)
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
        (int)context->render_snapshot.width,
        (int)context->render_snapshot.height,
        context->render_snapshot.pixels,
        &bitmap_info,
        DIB_RGB_COLORS,
        SRCCOPY);
    ShowCursor(TRUE);
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
    {
        RECT *frame_rect = (RECT *)lparam;
        RECT client_rect;
        int client_width;
        int client_height;
        uint32_t framebuffer_width = 0;
        uint32_t framebuffer_height = 0;

        if (context != NULL) {
            EnterCriticalSection(&context->framebuffer_lock);
            framebuffer_width = context->framebuffer.width;
            framebuffer_height = context->framebuffer.height;
            LeaveCriticalSection(&context->framebuffer_lock);
        }

        GetClientRect(window_handle, &client_rect);
        client_width = client_rect.right - client_rect.left;
        client_height = client_rect.bottom - client_rect.top;

        if (frame_rect != NULL && framebuffer_width > 0 && framebuffer_height > 0 && client_width > 0 && client_height > 0) {
            /* only invalidate the patched area (scaled to client coordinates) instead of a full-window redraw */
            RECT invalid_rect;
            invalid_rect.left = (LONG)((int64_t)frame_rect->left * client_width / (int)framebuffer_width);
            invalid_rect.top = (LONG)((int64_t)frame_rect->top * client_height / (int)framebuffer_height);
            invalid_rect.right = (LONG)((int64_t)frame_rect->right * client_width / (int)framebuffer_width) + 1;
            invalid_rect.bottom = (LONG)((int64_t)frame_rect->bottom * client_height / (int)framebuffer_height) + 1;
            InvalidateRect(window_handle, &invalid_rect, FALSE);
        } else {
            InvalidateRect(window_handle, NULL, FALSE);
        }

        free(frame_rect);
        return 0;
    }

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

/* posts a patched-rect notification to the UI thread; NULL rect means "invalidate everything" */
static void receiver_post_frame_ready(receiver_context_t *context, uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    RECT *rect = (RECT *)malloc(sizeof(RECT));

    if (rect == NULL) {
        PostMessage(context->window_handle, SS_APP_FRAME_READY, 0, 0);
        return;
    }

    rect->left = (LONG)x;
    rect->top = (LONG)y;
    rect->right = (LONG)(x + width);
    rect->bottom = (LONG)(y + height);
    PostMessage(context->window_handle, SS_APP_FRAME_READY, 0, (LPARAM)rect);
}

/* one of a pool of N worker threads (N = CPU core count) draining receiver_io_thread's decode queue */
static DWORD WINAPI receiver_decode_worker_thread(LPVOID parameter)
{
    receiver_context_t *context = (receiver_context_t *)parameter;
    HRESULT com_result = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    int com_initialized = SUCCEEDED(com_result);

    for (;;) {
        receiver_decode_job_t *job = receiver_decode_queue_pop_wait(&context->decode_queue, 1000);
        uint8_t *decoded_pixels = NULL;
        uint32_t decoded_width = 0;
        uint32_t decoded_height = 0;
        uint32_t decoded_stride = 0;
        uint64_t stage_start;

        if (job == NULL) {
            if (receiver_decode_queue_is_shutdown(&context->decode_queue)) {
                break;
            }
            continue;
        }

        stage_start = context->debug_stats != NULL ? ss_win_get_perf_counter() : 0;
        if (ss_jpeg_decode_to_bgra(job->jpeg_data, job->jpeg_size, &decoded_pixels, &decoded_width, &decoded_height, &decoded_stride) == 0) {
            if (context->debug_stats != NULL) {
                receiver_debug_stats_add(context->debug_stats, RECEIVER_DEBUG_STAGE_DECODE, ss_win_perf_counter_to_ms(stage_start, ss_win_get_perf_counter()));
            }

            /* patches may be applied slightly out of arrival order across worker threads -- accepted as
               low-risk since regions from one capture cycle don't overlap and any visible reordering is
               self-correcting on the next update; only FULL_FRAME messages need strict ordering */
            stage_start = context->debug_stats != NULL ? ss_win_get_perf_counter() : 0;
            EnterCriticalSection(&context->framebuffer_lock);
            receiver_framebuffer_patch(&context->framebuffer, job->x, job->y, decoded_width, decoded_height, decoded_pixels, decoded_stride);
            LeaveCriticalSection(&context->framebuffer_lock);
            if (context->debug_stats != NULL) {
                receiver_debug_stats_add(context->debug_stats, RECEIVER_DEBUG_STAGE_FRAMEBUFFER, ss_win_perf_counter_to_ms(stage_start, ss_win_get_perf_counter()));
            }

            receiver_post_frame_ready(context, job->x, job->y, decoded_width, decoded_height);
        }

        free(decoded_pixels);
        receiver_decode_job_free(job);
    }

    if (com_initialized) {
        CoUninitialize();
    }
    return 0;
}

/* receives header+payload and either applies FULL_FRAME synchronously (rare, connect/resize -- must never
   race a later region patch) or hands UPDATE_REGION payloads off to the decode worker pool */
static DWORD WINAPI receiver_io_thread(LPVOID parameter)
{
    receiver_context_t *context = (receiver_context_t *)parameter;
    HRESULT com_result;
    uint64_t last_debug_print_tick = ss_win_get_tick_count64();

    com_result = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(com_result)) {
        goto cleanup;
    }

    if (receiver_open_transport(&context->transport, &context->config) != 0) {
        goto cleanup;
    }

    while (WaitForSingleObject(context->stop_event, 0) != WAIT_OBJECT_0) {
        ss_message_header_t header;
        uint8_t *jpeg_data;
        uint32_t payload_size;
        uint64_t stage_start;
        uint64_t now;

        stage_start = context->debug_stats != NULL ? ss_win_get_perf_counter() : 0;
        if (ss_transport_recv_exact(&context->transport, &header, sizeof(header)) != 0) {
            goto cleanup;
        }
        if (context->debug_stats != NULL) {
            receiver_debug_stats_add(context->debug_stats, RECEIVER_DEBUG_STAGE_RECV_HEADER, ss_win_perf_counter_to_ms(stage_start, ss_win_get_perf_counter()));
        }

        if (!ss_protocol_validate_header(&header)) {
            goto cleanup;
        }

        payload_size = ntohl(header.payload_size);
        jpeg_data = (uint8_t *)malloc(payload_size);
        if (jpeg_data == NULL) {
            goto cleanup;
        }

        stage_start = context->debug_stats != NULL ? ss_win_get_perf_counter() : 0;
        if (ss_transport_recv_exact(&context->transport, jpeg_data, payload_size) != 0) {
            free(jpeg_data);
            goto cleanup;
        }
        if (context->debug_stats != NULL) {
            receiver_debug_stats_add(context->debug_stats, RECEIVER_DEBUG_STAGE_RECV_PAYLOAD, ss_win_perf_counter_to_ms(stage_start, ss_win_get_perf_counter()));
        }

        if (ntohs(header.type) == SS_MESSAGE_FULL_FRAME || context->framebuffer.pixels == NULL) {
            uint8_t *decoded_pixels = NULL;
            uint32_t decoded_width = 0;
            uint32_t decoded_height = 0;
            uint32_t decoded_stride = 0;

            stage_start = context->debug_stats != NULL ? ss_win_get_perf_counter() : 0;
            if (ss_jpeg_decode_to_bgra(jpeg_data, payload_size, &decoded_pixels, &decoded_width, &decoded_height, &decoded_stride) == 0) {
                if (context->debug_stats != NULL) {
                    receiver_debug_stats_add(context->debug_stats, RECEIVER_DEBUG_STAGE_DECODE, ss_win_perf_counter_to_ms(stage_start, ss_win_get_perf_counter()));
                }

                stage_start = context->debug_stats != NULL ? ss_win_get_perf_counter() : 0;
                EnterCriticalSection(&context->framebuffer_lock);
                receiver_framebuffer_replace(&context->framebuffer, decoded_pixels, decoded_width, decoded_height, decoded_stride);
                LeaveCriticalSection(&context->framebuffer_lock);
                if (context->debug_stats != NULL) {
                    receiver_debug_stats_add(context->debug_stats, RECEIVER_DEBUG_STAGE_FRAMEBUFFER, ss_win_perf_counter_to_ms(stage_start, ss_win_get_perf_counter()));
                }

                receiver_post_frame_ready(context, 0, 0, decoded_width, decoded_height);
            }
            free(decoded_pixels);
            free(jpeg_data);
        } else {
            receiver_decode_job_t *job = (receiver_decode_job_t *)malloc(sizeof(receiver_decode_job_t));
            if (job == NULL) {
                free(jpeg_data);
            } else {
                job->next = NULL;
                job->jpeg_data = jpeg_data;
                job->jpeg_size = payload_size;
                job->x = ntohl(header.x);
                job->y = ntohl(header.y);
                receiver_decode_queue_push(&context->decode_queue, job);
            }
        }

        if (context->debug_stats != NULL) {
            now = ss_win_get_tick_count64();
            if (now - last_debug_print_tick >= 1000u) {
                receiver_print_debug_stats(context->debug_stats);
                last_debug_print_tick = now;
            }
        }
    }

cleanup:
    receiver_decode_queue_signal_shutdown(&context->decode_queue);
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
    receiver_debug_stats_t debug_stats;
    HWND window_handle;
    HANDLE *decode_threads;
    DWORD decode_thread_count;
    DWORD i;
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
    context.debug_stats = NULL;
    receiver_decode_queue_init(&context.decode_queue);
    if (config.debug_enabled) {
        receiver_debug_stats_init(&debug_stats);
        context.debug_stats = &debug_stats;
    }

    {
        SYSTEM_INFO system_info;
        GetSystemInfo(&system_info);
        decode_thread_count = system_info.dwNumberOfProcessors;
        if (decode_thread_count == 0) {
            decode_thread_count = 1;
        }
    }

    decode_threads = (HANDLE *)calloc(decode_thread_count, sizeof(HANDLE));
    if (decode_threads == NULL) {
        printf("Unable to allocate the decode worker thread table.\n");
        receiver_decode_queue_destroy(&context.decode_queue);
        DeleteCriticalSection(&context.framebuffer_lock);
        CloseHandle(context.stop_event);
        if (config.debug_enabled) {
            receiver_debug_stats_destroy(&debug_stats);
        }
        CoUninitialize();
        WSACleanup();
        return 1;
    }

    ZeroMemory(&window_class, sizeof(window_class));
    window_class.lpfnWndProc = receiver_window_proc;
    window_class.hInstance = GetModuleHandle(NULL);
    window_class.lpszClassName = SS_WINDOW_CLASS_NAME;
    window_class.hCursor = LoadCursor(NULL, IDC_ARROW);

    if (!RegisterClass(&window_class)) {
        printf("Unable to register window class.\n");
        free(decode_threads);
        receiver_decode_queue_destroy(&context.decode_queue);
        DeleteCriticalSection(&context.framebuffer_lock);
        CloseHandle(context.stop_event);
        if (config.debug_enabled) {
            receiver_debug_stats_destroy(&debug_stats);
        }
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
        free(decode_threads);
        receiver_decode_queue_destroy(&context.decode_queue);
        DeleteCriticalSection(&context.framebuffer_lock);
        CloseHandle(context.stop_event);
        if (config.debug_enabled) {
            receiver_debug_stats_destroy(&debug_stats);
        }
        CoUninitialize();
        WSACleanup();
        return 1;
    }

    context.window_handle = window_handle;
    context.io_thread = CreateThread(NULL, 0, receiver_io_thread, &context, 0, NULL);
    for (i = 0; i < decode_thread_count; ++i) {
        decode_threads[i] = CreateThread(NULL, 0, receiver_decode_worker_thread, &context, 0, NULL);
    }

    {
        int startup_failed = (context.io_thread == NULL);

        for (i = 0; i < decode_thread_count; ++i) {
            if (decode_threads[i] == NULL) {
                startup_failed = 1;
            }
        }

        if (startup_failed) {
            printf("Unable to create I/O/decode worker threads.\n");
            SetEvent(context.stop_event);
            receiver_decode_queue_signal_shutdown(&context.decode_queue);
            DestroyWindow(window_handle);

            if (context.io_thread != NULL) {
                WaitForSingleObject(context.io_thread, INFINITE);
                CloseHandle(context.io_thread);
            }
            for (i = 0; i < decode_thread_count; ++i) {
                if (decode_threads[i] != NULL) {
                    WaitForSingleObject(decode_threads[i], INFINITE);
                    CloseHandle(decode_threads[i]);
                }
            }

            free(decode_threads);
            receiver_decode_queue_destroy(&context.decode_queue);
            DeleteCriticalSection(&context.framebuffer_lock);
            CloseHandle(context.stop_event);
            if (config.debug_enabled) {
                receiver_debug_stats_destroy(&debug_stats);
            }
            CoUninitialize();
            WSACleanup();
            return 1;
        }
    }

    ShowWindow(window_handle, SW_SHOWDEFAULT);
    UpdateWindow(window_handle);

    while (GetMessage(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessage(&message);
    }

    WaitForSingleObject(context.io_thread, INFINITE);
    CloseHandle(context.io_thread);
    for (i = 0; i < decode_thread_count; ++i) {
        WaitForSingleObject(decode_threads[i], INFINITE);
        CloseHandle(decode_threads[i]);
    }
    free(decode_threads);
    receiver_decode_queue_destroy(&context.decode_queue);
    receiver_framebuffer_release(&context.framebuffer);
    receiver_framebuffer_release(&context.render_snapshot);
    DeleteCriticalSection(&context.framebuffer_lock);
    CloseHandle(context.stop_event);
    if (config.debug_enabled) {
        receiver_debug_stats_destroy(&debug_stats);
    }
    ss_jpeg_shutdown();
    CoUninitialize();
    WSACleanup();
    return exit_code;
}

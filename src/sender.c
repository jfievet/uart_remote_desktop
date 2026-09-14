#define WIN32_LEAN_AND_MEAN

#include "capture.h"
#include "input.h"
#include "protocol.h"
#include "transport.h"
#include "wic_jpeg.h"
#include "win_compat.h"

#include <windows.h>
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct sender_config {
    const char *host;
    unsigned short port;
    unsigned int interval_ms;
    unsigned int tile_size;
    int ber_mode;
    ss_transport_type_t transport_type;
    unsigned int com_port;
    unsigned long baud_rate;
    ss_jpeg_backend_t jpeg_backend;
    ss_capture_backend_t capture_backend;
    int debug_enabled;
} sender_config_t;

static void sender_print_usage(void)
{
    printf("Usage: sender [--tcp | --uart] --host <ip> --port <port> [--com <n>] [--speed <baud>] [--interval-ms <ms>] [--tile-size <pixels>] [--jpeg-backend windows|c] [--capture-backend gdi|dxgi] [--ber] [--debug]\n");
}

typedef enum sender_debug_stage {
    SENDER_DEBUG_STAGE_CAPTURE = 0,
    SENDER_DEBUG_STAGE_DIFF,
    SENDER_DEBUG_STAGE_ENCODE,
    SENDER_DEBUG_STAGE_SEND,
    SENDER_DEBUG_STAGE_COUNT
} sender_debug_stage_t;

typedef struct sender_debug_stats {
    CRITICAL_SECTION lock;
    double stage_total_ms[SENDER_DEBUG_STAGE_COUNT];
    uint64_t stage_count[SENDER_DEBUG_STAGE_COUNT];
} sender_debug_stats_t;

static const char *sender_debug_stage_names[SENDER_DEBUG_STAGE_COUNT] = { "capture", "diff", "encode", "send" };

static void sender_debug_stats_init(sender_debug_stats_t *stats)
{
    ZeroMemory(stats, sizeof(*stats));
    InitializeCriticalSection(&stats->lock);
}

static void sender_debug_stats_destroy(sender_debug_stats_t *stats)
{
    DeleteCriticalSection(&stats->lock);
}

static void sender_debug_stats_add(sender_debug_stats_t *stats, sender_debug_stage_t stage, double elapsed_ms)
{
    if (stats == NULL) {
        return;
    }

    EnterCriticalSection(&stats->lock);
    stats->stage_total_ms[stage] += elapsed_ms;
    stats->stage_count[stage] += 1;
    LeaveCriticalSection(&stats->lock);
}

static void sender_print_debug_stats(sender_debug_stats_t *stats)
{
    int stage;

    EnterCriticalSection(&stats->lock);
    printf("Debug stage timings (avg ms/call)");
    for (stage = 0; stage < SENDER_DEBUG_STAGE_COUNT; ++stage) {
        double average_ms = stats->stage_count[stage] > 0 ? stats->stage_total_ms[stage] / (double)stats->stage_count[stage] : 0.0;
        printf(" | %s : %.3f ms (%llu calls)", sender_debug_stage_names[stage], average_ms, (unsigned long long)stats->stage_count[stage]);
        stats->stage_total_ms[stage] = 0.0;
        stats->stage_count[stage] = 0;
    }
    printf("\n");
    fflush(stdout);
    LeaveCriticalSection(&stats->lock);
}

static DWORD WINAPI sender_input_thread(LPVOID parameter)
{
    ss_transport_t *transport = (ss_transport_t *)parameter;

    for (;;) {
        ss_message_header_t header;
        uint16_t message_type;

        if (ss_transport_recv_exact(transport, &header, sizeof(header)) != 0) {
            break;
        }

        if (!ss_protocol_validate_header(&header)) {
            break;
        }

        message_type = ntohs(header.type);

        if (message_type == SS_MESSAGE_MOUSE_EVENT) {
            ss_mouse_event_t payload;
            uint32_t x;
            uint32_t y;
            ss_mouse_action_t action;
            int32_t param;

            if (ntohl(header.payload_size) != sizeof(payload) || ss_transport_recv_exact(transport, &payload, sizeof(payload)) != 0) {
                break;
            }

            ss_protocol_decode_mouse_event(&payload, &x, &y, &action, &param);
            ss_input_inject_mouse_event(x, y, action, param);
        } else if (message_type == SS_MESSAGE_KEY_EVENT) {
            ss_key_event_t payload;
            uint32_t vk_code;
            uint32_t scan_code;
            ss_key_action_t action;
            uint32_t extended;

            if (ntohl(header.payload_size) != sizeof(payload) || ss_transport_recv_exact(transport, &payload, sizeof(payload)) != 0) {
                break;
            }

            ss_protocol_decode_key_event(&payload, &vk_code, &scan_code, &action, &extended);
            ss_input_inject_key_event(vk_code, scan_code, action, extended);
        } else {
            break;
        }
    }

    return 0;
}

static int sender_parse_args(int argc, char **argv, sender_config_t *config)
{
    int index;

    config->host = "127.0.0.1";
    config->port = 5000;
    config->interval_ms = 33;
    config->tile_size = 64;
    config->ber_mode = 0;
    config->transport_type = SS_TRANSPORT_TCP;
    config->com_port = 0;
    config->baud_rate = 3000000UL;
    config->jpeg_backend = SS_JPEG_BACKEND_WINDOWS;
    config->capture_backend = SS_CAPTURE_BACKEND_GDI;

    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--help") == 0 || strcmp(argv[index], "-h") == 0) {
            return 1;
        } else if (strcmp(argv[index], "--host") == 0 && index + 1 < argc) {
            config->host = argv[++index];
        } else if (strcmp(argv[index], "--port") == 0 && index + 1 < argc) {
            config->port = (unsigned short)atoi(argv[++index]);
        } else if (strcmp(argv[index], "--interval-ms") == 0 && index + 1 < argc) {
            config->interval_ms = (unsigned int)atoi(argv[++index]);
        } else if (strcmp(argv[index], "--tile-size") == 0 && index + 1 < argc) {
            config->tile_size = (unsigned int)atoi(argv[++index]);
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
        } else if (strcmp(argv[index], "--capture-backend") == 0 && index + 1 < argc) {
            if (ss_capture_parse_backend(argv[++index], &config->capture_backend) != 0) {
                return -1;
            }
        } else if (strcmp(argv[index], "--debug") == 0) {
            config->debug_enabled = 1;
        } else {
            return -1;
        }
    }

    if (config->host == NULL || config->port == 0 || config->interval_ms == 0 || config->tile_size == 0) {
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

static uint8_t *sender_build_message_buffer(ss_message_type_t type, uint32_t x, uint32_t y, uint32_t width, uint32_t height, const uint8_t *jpeg_data, size_t jpeg_size, size_t *out_message_size)
{
    ss_message_header_t header;
    uint8_t *buffer;

    ss_protocol_make_header(&header, type, x, y, width, height, (uint32_t)jpeg_size);

    /* one contiguous buffer so the send thread issues a single write instead of a separate header segment */
    *out_message_size = sizeof(header) + jpeg_size;
    buffer = (uint8_t *)malloc(*out_message_size);
    if (buffer == NULL) {
        return NULL;
    }

    memcpy(buffer, &header, sizeof(header));
    memcpy(buffer + sizeof(header), jpeg_data, jpeg_size);
    return buffer;
}

static int sender_is_tile_dirty(const ss_frame_t *current, const ss_frame_t *previous, uint32_t x, uint32_t y, uint32_t tile_width, uint32_t tile_height)
{
    uint32_t row;

    for (row = 0; row < tile_height; ++row) {
        const uint8_t *current_row = current->pixels + (size_t)(y + row) * current->stride + (size_t)x * 4u;
        const uint8_t *previous_row = previous->pixels + (size_t)(y + row) * previous->stride + (size_t)x * 4u;
        if (memcmp(current_row, previous_row, (size_t)tile_width * 4u) != 0) {
            return 1;
        }
    }

    return 0;
}

/* ---- region job queue: capture thread -> pool of encode worker threads ---- */

typedef struct sender_region_job {
    struct sender_region_job *next;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint8_t *pixels; /* tightly packed BGRA (stride == width*4), owned by the job until freed */
    ss_message_type_t message_type;
} sender_region_job_t;

typedef struct sender_region_queue {
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE not_empty;
    sender_region_job_t *head;
    sender_region_job_t *tail;
    int shutdown;
} sender_region_queue_t;

static void sender_region_queue_init(sender_region_queue_t *queue)
{
    ZeroMemory(queue, sizeof(*queue));
    InitializeCriticalSection(&queue->lock);
    InitializeConditionVariable(&queue->not_empty);
}

static void sender_region_job_free(sender_region_job_t *job)
{
    if (job != NULL) {
        free(job->pixels);
        free(job);
    }
}

static void sender_region_queue_destroy(sender_region_queue_t *queue)
{
    sender_region_job_t *job;

    EnterCriticalSection(&queue->lock);
    job = queue->head;
    queue->head = NULL;
    queue->tail = NULL;
    LeaveCriticalSection(&queue->lock);

    while (job != NULL) {
        sender_region_job_t *next = job->next;
        sender_region_job_free(job);
        job = next;
    }

    DeleteCriticalSection(&queue->lock);
}

/* full-frame jobs jump the queue so they are encoded/sent ahead of any pending region updates */
static void sender_region_queue_push(sender_region_queue_t *queue, sender_region_job_t *job, int priority)
{
    job->next = NULL;

    EnterCriticalSection(&queue->lock);
    if (priority || queue->head == NULL) {
        job->next = queue->head;
        queue->head = job;
        if (queue->tail == NULL) {
            queue->tail = job;
        }
    } else {
        queue->tail->next = job;
        queue->tail = job;
    }
    LeaveCriticalSection(&queue->lock);
    WakeConditionVariable(&queue->not_empty);
}

static void sender_region_queue_signal_shutdown(sender_region_queue_t *queue)
{
    EnterCriticalSection(&queue->lock);
    queue->shutdown = 1;
    LeaveCriticalSection(&queue->lock);
    WakeAllConditionVariable(&queue->not_empty);
}

static int sender_region_queue_is_shutdown(sender_region_queue_t *queue)
{
    int result;

    EnterCriticalSection(&queue->lock);
    result = queue->shutdown;
    LeaveCriticalSection(&queue->lock);
    return result;
}

static sender_region_job_t *sender_region_queue_pop_wait(sender_region_queue_t *queue, DWORD timeout_ms)
{
    sender_region_job_t *job = NULL;

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

/* copies frame->pixels and enqueues it as a top-priority full-frame job -- frame itself is left untouched */
static int sender_publish_full_frame(sender_region_queue_t *queue, const ss_frame_t *frame)
{
    size_t frame_size = (size_t)frame->stride * (size_t)frame->height;
    uint8_t *pixels_copy = (uint8_t *)malloc(frame_size);
    sender_region_job_t *job;

    if (pixels_copy == NULL) {
        return -1;
    }
    memcpy(pixels_copy, frame->pixels, frame_size);

    job = (sender_region_job_t *)malloc(sizeof(sender_region_job_t));
    if (job == NULL) {
        free(pixels_copy);
        return -1;
    }

    job->next = NULL;
    job->x = 0;
    job->y = 0;
    job->width = frame->width;
    job->height = frame->height;
    job->pixels = pixels_copy;
    job->message_type = SS_MESSAGE_FULL_FRAME;
    sender_region_queue_push(queue, job, 1);
    return 0;
}

/* merges adjacent dirty grid cells into larger rectangles (row-wise run, then matching-span column-wise merge)
   so fewer, larger regions get encoded/sent instead of one JPEG per fixed tile */
static void sender_scan_and_publish_dirty_regions(sender_region_queue_t *region_queue, const ss_frame_t *current, const ss_frame_t *previous, uint32_t tile_size)
{
    uint32_t columns = (current->width + tile_size - 1u) / tile_size;
    uint32_t rows = (current->height + tile_size - 1u) / tile_size;
    uint8_t *dirty;
    uint8_t *consumed;
    uint32_t row;
    uint32_t column;

    if (columns == 0 || rows == 0) {
        return;
    }

    dirty = (uint8_t *)calloc((size_t)columns * (size_t)rows, sizeof(uint8_t));
    consumed = (uint8_t *)calloc((size_t)columns * (size_t)rows, sizeof(uint8_t));
    if (dirty == NULL || consumed == NULL) {
        free(dirty);
        free(consumed);
        return;
    }

    for (row = 0; row < rows; ++row) {
        for (column = 0; column < columns; ++column) {
            uint32_t x = column * tile_size;
            uint32_t y = row * tile_size;
            uint32_t cell_width = current->width - x;
            uint32_t cell_height = current->height - y;

            if (cell_width > tile_size) {
                cell_width = tile_size;
            }
            if (cell_height > tile_size) {
                cell_height = tile_size;
            }

            dirty[(size_t)row * columns + column] = (uint8_t)sender_is_tile_dirty(current, previous, x, y, cell_width, cell_height);
        }
    }

    for (row = 0; row < rows; ++row) {
        for (column = 0; column < columns; ++column) {
            uint32_t run_end;
            uint32_t row_end;
            uint32_t rect_x;
            uint32_t rect_y;
            uint32_t rect_width;
            uint32_t rect_height;
            uint8_t *region_pixels;

            if (!dirty[(size_t)row * columns + column] || consumed[(size_t)row * columns + column]) {
                continue;
            }

            run_end = column + 1u;
            while (run_end < columns && dirty[(size_t)row * columns + run_end] && !consumed[(size_t)row * columns + run_end]) {
                ++run_end;
            }

            /* extend the run downward while subsequent rows have the exact same span still unconsumed */
            row_end = row + 1u;
            for (;;) {
                uint32_t check_column;
                int matches = row_end < rows;

                for (check_column = column; matches && check_column < run_end; ++check_column) {
                    if (!dirty[(size_t)row_end * columns + check_column] || consumed[(size_t)row_end * columns + check_column]) {
                        matches = 0;
                    }
                }

                if (!matches) {
                    break;
                }
                ++row_end;
            }

            {
                uint32_t mark_row;
                uint32_t mark_column;
                for (mark_row = row; mark_row < row_end; ++mark_row) {
                    for (mark_column = column; mark_column < run_end; ++mark_column) {
                        consumed[(size_t)mark_row * columns + mark_column] = 1;
                    }
                }
            }

            rect_x = column * tile_size;
            rect_y = row * tile_size;
            rect_width = current->width - rect_x;
            rect_height = current->height - rect_y;
            if (rect_width > (run_end - column) * tile_size) {
                rect_width = (run_end - column) * tile_size;
            }
            if (rect_height > (row_end - row) * tile_size) {
                rect_height = (row_end - row) * tile_size;
            }

            region_pixels = (uint8_t *)malloc((size_t)rect_width * (size_t)rect_height * 4u);
            if (region_pixels == NULL) {
                continue;
            }

            if (ss_frame_copy_region(current, rect_x, rect_y, rect_width, rect_height, region_pixels, rect_width * 4u) != 0) {
                free(region_pixels);
                continue;
            }

            {
                sender_region_job_t *job = (sender_region_job_t *)malloc(sizeof(sender_region_job_t));
                if (job == NULL) {
                    free(region_pixels);
                    continue;
                }
                job->next = NULL;
                job->x = rect_x;
                job->y = rect_y;
                job->width = rect_width;
                job->height = rect_height;
                job->pixels = region_pixels;
                job->message_type = SS_MESSAGE_UPDATE_REGION;
                sender_region_queue_push(region_queue, job, 0);
            }
        }
    }

    free(dirty);
    free(consumed);
}

/* ---- send job queue: pool of encode worker threads -> single dedicated send thread ---- */

typedef struct sender_send_job {
    struct sender_send_job *next;
    uint8_t *buffer;
    size_t size;
} sender_send_job_t;

typedef struct sender_send_queue {
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE not_empty;
    sender_send_job_t *head;
    sender_send_job_t *tail;
    int shutdown;
} sender_send_queue_t;

static void sender_send_queue_init(sender_send_queue_t *queue)
{
    ZeroMemory(queue, sizeof(*queue));
    InitializeCriticalSection(&queue->lock);
    InitializeConditionVariable(&queue->not_empty);
}

static void sender_send_job_free(sender_send_job_t *job)
{
    if (job != NULL) {
        free(job->buffer);
        free(job);
    }
}

static void sender_send_queue_destroy(sender_send_queue_t *queue)
{
    sender_send_job_t *job;

    EnterCriticalSection(&queue->lock);
    job = queue->head;
    queue->head = NULL;
    queue->tail = NULL;
    LeaveCriticalSection(&queue->lock);

    while (job != NULL) {
        sender_send_job_t *next = job->next;
        sender_send_job_free(job);
        job = next;
    }

    DeleteCriticalSection(&queue->lock);
}

static void sender_send_queue_push(sender_send_queue_t *queue, sender_send_job_t *job, int priority)
{
    job->next = NULL;

    EnterCriticalSection(&queue->lock);
    if (priority || queue->head == NULL) {
        job->next = queue->head;
        queue->head = job;
        if (queue->tail == NULL) {
            queue->tail = job;
        }
    } else {
        queue->tail->next = job;
        queue->tail = job;
    }
    LeaveCriticalSection(&queue->lock);
    WakeConditionVariable(&queue->not_empty);
}

static void sender_send_queue_signal_shutdown(sender_send_queue_t *queue)
{
    EnterCriticalSection(&queue->lock);
    queue->shutdown = 1;
    LeaveCriticalSection(&queue->lock);
    WakeAllConditionVariable(&queue->not_empty);
}

static int sender_send_queue_is_shutdown(sender_send_queue_t *queue)
{
    int result;

    EnterCriticalSection(&queue->lock);
    result = queue->shutdown;
    LeaveCriticalSection(&queue->lock);
    return result;
}

static sender_send_job_t *sender_send_queue_pop_wait(sender_send_queue_t *queue, DWORD timeout_ms)
{
    sender_send_job_t *job = NULL;

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

/* non-blocking: used to opportunistically batch whatever else is already queued into one send */
static sender_send_job_t *sender_send_queue_try_pop(sender_send_queue_t *queue)
{
    sender_send_job_t *job = NULL;

    EnterCriticalSection(&queue->lock);
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

static void sender_print_stats(uint64_t total_bytes, uint64_t start_tick, uint64_t last_tick, uint64_t bytes_since_last, uint64_t cells_since_last)
{
    uint64_t now = ss_win_get_tick_count64();
    uint64_t elapsed_ms = now - start_tick;
    uint64_t window_ms = now - last_tick;
    double total_seconds = elapsed_ms > 0 ? (double)elapsed_ms / 1000.0 : 0.0;
    double current_bytes_per_second = window_ms > 0 ? ((double)bytes_since_last * 1000.0) / (double)window_ms : 0.0;
    double current_bits_per_second = current_bytes_per_second * 8.0;
    unsigned int hours = (unsigned int)(total_seconds / 3600.0);
    unsigned int minutes = (unsigned int)((total_seconds - (hours * 3600.0)) / 60.0);
    unsigned int seconds = (unsigned int)(total_seconds) % 60u;

    printf("Current throughput : %.2f bytes/s (%.2f bits/s) | Updated cells : %llu | Total transmitted : %.0f bytes | Session duration : %02u:%02u:%02u\n",
        current_bytes_per_second,
        current_bits_per_second,
        (unsigned long long)cells_since_last,
        (double)total_bytes,
        hours,
        minutes,
        seconds);
    fflush(stdout);
}

typedef struct sender_capture_context {
    sender_config_t config;
    sender_region_queue_t *region_queue;
    ss_frame_t initial_frame; /* ownership transferred in from main; becomes the first "previous frame" */
    volatile LONG *stop_requested;
    sender_debug_stats_t *debug_stats;
} sender_capture_context_t;

static DWORD WINAPI sender_capture_thread(LPVOID parameter)
{
    sender_capture_context_t *context = (sender_capture_context_t *)parameter;
    ss_frame_t previous_frame = context->initial_frame;

    while (*context->stop_requested == 0) {
        uint64_t cycle_start = ss_win_get_tick_count64();
        ss_frame_t current_frame;
        uint64_t cycle_elapsed;
        uint64_t stage_start;

        ZeroMemory(&current_frame, sizeof(current_frame));
        stage_start = context->debug_stats != NULL ? ss_win_get_perf_counter() : 0;
        if (ss_capture_primary_screen(&current_frame) != 0) {
            InterlockedExchange(context->stop_requested, 1);
            break;
        }
        if (context->debug_stats != NULL) {
            sender_debug_stats_add(context->debug_stats, SENDER_DEBUG_STAGE_CAPTURE, ss_win_perf_counter_to_ms(stage_start, ss_win_get_perf_counter()));
        }

        if (current_frame.width != previous_frame.width || current_frame.height != previous_frame.height) {
            if (sender_publish_full_frame(context->region_queue, &current_frame) != 0) {
                ss_frame_release(&current_frame);
                InterlockedExchange(context->stop_requested, 1);
                break;
            }
        } else {
            stage_start = context->debug_stats != NULL ? ss_win_get_perf_counter() : 0;
            sender_scan_and_publish_dirty_regions(context->region_queue, &current_frame, &previous_frame, context->config.tile_size);
            if (context->debug_stats != NULL) {
                sender_debug_stats_add(context->debug_stats, SENDER_DEBUG_STAGE_DIFF, ss_win_perf_counter_to_ms(stage_start, ss_win_get_perf_counter()));
            }
        }

        ss_frame_release(&previous_frame);
        previous_frame = current_frame;

        cycle_elapsed = ss_win_get_tick_count64() - cycle_start;
        if (cycle_elapsed < context->config.interval_ms) {
            Sleep(context->config.interval_ms - (DWORD)cycle_elapsed); /* never pad delay on top of an already-slow cycle */
        }
    }

    ss_frame_release(&previous_frame);
    InterlockedExchange(context->stop_requested, 1);
    return 0;
}

typedef struct sender_encode_worker_context {
    sender_region_queue_t *region_queue;
    sender_send_queue_t *send_queue;
    sender_debug_stats_t *debug_stats;
} sender_encode_worker_context_t;

/* one of a pool of N worker threads (N = CPU core count); a failed encode just drops that region -- only
   an actual transport send failure (in sender_send_thread) is treated as fatal for the whole session */
static DWORD WINAPI sender_encode_worker_thread(LPVOID parameter)
{
    sender_encode_worker_context_t *context = (sender_encode_worker_context_t *)parameter;

    for (;;) {
        sender_region_job_t *job = sender_region_queue_pop_wait(context->region_queue, 1000);
        uint8_t *jpeg_data = NULL;
        size_t jpeg_size = 0;
        uint64_t stage_start;
        int encode_ok;

        if (job == NULL) {
            if (sender_region_queue_is_shutdown(context->region_queue)) {
                break;
            }
            continue;
        }

        stage_start = context->debug_stats != NULL ? ss_win_get_perf_counter() : 0;
        encode_ok = ss_jpeg_encode_bgra(job->pixels, job->width, job->height, job->width * 4u, 0.80f, &jpeg_data, &jpeg_size) == 0;
        if (context->debug_stats != NULL) {
            sender_debug_stats_add(context->debug_stats, SENDER_DEBUG_STAGE_ENCODE, ss_win_perf_counter_to_ms(stage_start, ss_win_get_perf_counter()));
        }

        if (encode_ok) {
            size_t message_size;
            uint8_t *message = sender_build_message_buffer(job->message_type, job->x, job->y, job->width, job->height, jpeg_data, jpeg_size, &message_size);

            if (message != NULL) {
                sender_send_job_t *send_job = (sender_send_job_t *)malloc(sizeof(sender_send_job_t));
                if (send_job != NULL) {
                    send_job->next = NULL;
                    send_job->buffer = message;
                    send_job->size = message_size;
                    sender_send_queue_push(context->send_queue, send_job, job->message_type == SS_MESSAGE_FULL_FRAME);
                } else {
                    free(message);
                }
            }
        }

        free(jpeg_data);
        sender_region_job_free(job);
    }

    return 0;
}

#define SENDER_SEND_BATCH_MAX_BYTES (256u * 1024u)

typedef struct sender_send_context {
    ss_transport_t *transport;
    sender_send_queue_t *send_queue;
    volatile LONG *stop_requested;
    uint64_t total_bytes_sent;
    sender_debug_stats_t *debug_stats;
} sender_send_context_t;

static DWORD WINAPI sender_send_thread(LPVOID parameter)
{
    sender_send_context_t *context = (sender_send_context_t *)parameter;
    uint64_t start_tick = ss_win_get_tick_count64();
    uint64_t last_stat_tick = start_tick;
    uint64_t bytes_since_last_stat = 0;
    uint64_t cells_since_last_stat = 0;

    for (;;) {
        sender_send_job_t *job = sender_send_queue_pop_wait(context->send_queue, 1000);
        uint64_t now;

        if (job == NULL) {
            if (sender_send_queue_is_shutdown(context->send_queue)) {
                break;
            }
        } else {
            uint8_t *batch_buffer = (uint8_t *)malloc(job->size);
            size_t batch_size;
            uint64_t message_count;
            uint64_t stage_start;
            int send_failed;

            if (batch_buffer == NULL) {
                sender_send_job_free(job);
                goto check_stats;
            }

            memcpy(batch_buffer, job->buffer, job->size);
            batch_size = job->size;
            message_count = 1;
            sender_send_job_free(job);

            /* opportunistically coalesce whatever else is already queued into one write, bounded so a
               burst of many ready messages can't grow the batch (and its latency) without limit */
            while (batch_size < SENDER_SEND_BATCH_MAX_BYTES) {
                sender_send_job_t *more = sender_send_queue_try_pop(context->send_queue);
                uint8_t *grown;

                if (more == NULL) {
                    break;
                }

                grown = (uint8_t *)realloc(batch_buffer, batch_size + more->size);
                if (grown == NULL) {
                    sender_send_job_free(more);
                    break;
                }

                batch_buffer = grown;
                memcpy(batch_buffer + batch_size, more->buffer, more->size);
                batch_size += more->size;
                ++message_count;
                sender_send_job_free(more);
            }

            stage_start = context->debug_stats != NULL ? ss_win_get_perf_counter() : 0;
            send_failed = ss_transport_send_all(context->transport, batch_buffer, batch_size) != 0;
            if (context->debug_stats != NULL) {
                sender_debug_stats_add(context->debug_stats, SENDER_DEBUG_STAGE_SEND, ss_win_perf_counter_to_ms(stage_start, ss_win_get_perf_counter()));
            }
            free(batch_buffer);

            if (send_failed) {
                InterlockedExchange(context->stop_requested, 1);
                break;
            }

            context->total_bytes_sent += batch_size;
            bytes_since_last_stat += batch_size;
            cells_since_last_stat += message_count;
        }

check_stats:
        now = ss_win_get_tick_count64();
        if (now - last_stat_tick >= 1000u) {
            sender_print_stats(context->total_bytes_sent, start_tick, last_stat_tick, bytes_since_last_stat, cells_since_last_stat);
            if (context->debug_stats != NULL) {
                sender_print_debug_stats(context->debug_stats);
            }
            bytes_since_last_stat = 0;
            cells_since_last_stat = 0;
            last_stat_tick = now;
        }
    }

    sender_print_stats(context->total_bytes_sent, start_tick, last_stat_tick, bytes_since_last_stat, cells_since_last_stat);
    return 0;
}


#define SS_BER_CHUNK_SIZE 4096u
#define SS_BER_WINDOW_SECONDS 5u
#define SS_BER_DESYNC_RATE_THRESHOLD 0.15
#define SS_BER_RESYNC_GOOD_RATE 0.05
#define SS_BER_RESYNC_SEARCH_LIMIT 256u

typedef struct ss_prbs_state {
    uint32_t lfsr;
} ss_prbs_state_t;

static void ss_prbs_init(ss_prbs_state_t *state)
{
    state->lfsr = 0xACE1u; /* arbitrary fixed nonzero seed -- shared by every generator/verifier instance */
}

static uint8_t ss_prbs_next_byte(ss_prbs_state_t *state)
{
    uint8_t byte = 0;
    int bit_index;

    for (bit_index = 0; bit_index < 8; ++bit_index) {
        uint32_t feedback = ((state->lfsr >> 31) ^ (state->lfsr >> 21) ^ (state->lfsr >> 1) ^ state->lfsr) & 1u;
        state->lfsr = (state->lfsr << 1) | feedback;
        byte = (uint8_t)((byte << 1) | feedback);
    }

    return byte;
}

static void ss_prbs_fill(ss_prbs_state_t *state, uint8_t *buffer, size_t length)
{
    size_t index;

    for (index = 0; index < length; ++index) {
        buffer[index] = ss_prbs_next_byte(state);
    }
}

typedef struct sender_ber_context {
    ss_transport_t *transport;
    volatile LONG stop_requested;
    CRITICAL_SECTION lock;
    uint64_t total_bits_sent;
    uint64_t total_bits_checked;
    uint64_t total_bit_errors;
    uint64_t total_resyncs;
    uint64_t window_bytes[SS_BER_WINDOW_SECONDS];
    int window_pos;
} sender_ber_context_t;

static DWORD WINAPI sender_ber_writer_thread(LPVOID parameter)
{
    sender_ber_context_t *context = (sender_ber_context_t *)parameter;
    ss_prbs_state_t generator;
    uint8_t buffer[SS_BER_CHUNK_SIZE];

    ss_prbs_init(&generator);

    while (context->stop_requested == 0) {
        ss_prbs_fill(&generator, buffer, sizeof(buffer));

        if (ss_transport_send_all(context->transport, buffer, sizeof(buffer)) != 0) {
            InterlockedExchange(&context->stop_requested, 1);
            break;
        }

        EnterCriticalSection(&context->lock);
        context->total_bits_sent += (uint64_t)sizeof(buffer) * 8u;
        context->window_bytes[context->window_pos] += sizeof(buffer);
        LeaveCriticalSection(&context->lock);
    }

    return 0;
}

static uint64_t ss_prbs_count_errors(ss_prbs_state_t state, const uint8_t *buffer, size_t length)
{
    uint64_t errors = 0;
    size_t index;

    for (index = 0; index < length; ++index) {
        uint8_t expected = ss_prbs_next_byte(&state);
        uint8_t diff = (uint8_t)(expected ^ buffer[index]);
        errors += (uint64_t)__builtin_popcount(diff);
    }

    return errors;
}

static DWORD WINAPI sender_ber_reader_thread(LPVOID parameter)
{
    sender_ber_context_t *context = (sender_ber_context_t *)parameter;
    ss_prbs_state_t verifier;
    uint8_t buffer[SS_BER_CHUNK_SIZE];

    ss_prbs_init(&verifier);

    while (context->stop_requested == 0) {
        ss_prbs_state_t chunk_start_state = verifier;
        uint64_t chunk_errors;
        double chunk_rate;
        uint32_t resync_shift = 0;

        if (ss_transport_recv_exact(context->transport, buffer, sizeof(buffer)) != 0) {
            InterlockedExchange(&context->stop_requested, 1);
            break;
        }

        chunk_errors = ss_prbs_count_errors(verifier, buffer, sizeof(buffer));
        chunk_rate = (double)chunk_errors / ((double)sizeof(buffer) * 8.0);

        if (chunk_rate > SS_BER_DESYNC_RATE_THRESHOLD) {
            /* likely a byte was dropped/inserted on the link -- search a small forward window for a shift that realigns the stream */
            uint32_t shift;
            uint32_t best_shift = 0;
            uint64_t best_errors = chunk_errors;

            for (shift = 1; shift <= SS_BER_RESYNC_SEARCH_LIMIT; ++shift) {
                ss_prbs_state_t trial = chunk_start_state;
                uint64_t trial_errors;
                uint32_t skip;

                for (skip = 0; skip < shift; ++skip) {
                    ss_prbs_next_byte(&trial);
                }

                trial_errors = ss_prbs_count_errors(trial, buffer, sizeof(buffer));
                if (trial_errors < best_errors) {
                    best_errors = trial_errors;
                    best_shift = shift;
                }

                if (((double)trial_errors / ((double)sizeof(buffer) * 8.0)) < SS_BER_RESYNC_GOOD_RATE) {
                    break;
                }
            }

            if (best_shift > 0 && ((double)best_errors / ((double)sizeof(buffer) * 8.0)) < SS_BER_DESYNC_RATE_THRESHOLD) {
                resync_shift = best_shift;
                chunk_errors = best_errors;

                EnterCriticalSection(&context->lock);
                context->total_resyncs += 1;
                LeaveCriticalSection(&context->lock);
            } else {
                /* no byte-shift fixes it -- likely a bit-level corruption, not a lost/inserted byte; dump a sample for diagnosis, throttled to avoid flooding */
                static uint64_t last_dump_tick = 0;
                uint64_t now = ss_win_get_tick_count64();

                if (now - last_dump_tick > 2000) {
                    ss_prbs_state_t dump_state = chunk_start_state;
                    int k;

                    printf("BER desync sample - expected:");
                    for (k = 0; k < 16; ++k) {
                        printf(" %02X", ss_prbs_next_byte(&dump_state));
                    }
                    printf(" | actual:");
                    for (k = 0; k < 16; ++k) {
                        printf(" %02X", buffer[k]);
                    }
                    printf("\n");
                    fflush(stdout);
                    last_dump_tick = now;
                }
            }
        }

        /* advance the live verifier past the bytes consumed for this chunk (plus any lost bytes skipped by a resync) */
        verifier = chunk_start_state;
        {
            uint32_t skip;
            size_t index;

            for (skip = 0; skip < resync_shift; ++skip) {
                ss_prbs_next_byte(&verifier);
            }
            for (index = 0; index < sizeof(buffer); ++index) {
                ss_prbs_next_byte(&verifier);
            }
        }

        EnterCriticalSection(&context->lock);
        context->total_bit_errors += chunk_errors;
        context->total_bits_checked += (uint64_t)sizeof(buffer) * 8u;
        LeaveCriticalSection(&context->lock);
    }

    return 0;
}

static void sender_run_ber_mode(ss_transport_t *transport)
{
    sender_ber_context_t context;
    HANDLE writer_thread;
    HANDLE reader_thread;
    uint64_t previous_bits_checked = 0;

    ZeroMemory(&context, sizeof(context));
    context.transport = transport;
    InitializeCriticalSection(&context.lock);

    printf("BER test mode: transmitting PRBS data and verifying loopback...\n");

    writer_thread = CreateThread(NULL, 0, sender_ber_writer_thread, &context, 0, NULL);
    reader_thread = CreateThread(NULL, 0, sender_ber_reader_thread, &context, 0, NULL);

    if (writer_thread == NULL || reader_thread == NULL) {
        printf("Unable to create BER worker threads.\n");
        InterlockedExchange(&context.stop_requested, 1);
    }

    while (context.stop_requested == 0) {
        uint64_t window_total;
        double average_bps;
        double error_rate;
        int i;
        int nothing_received_back;

        if (writer_thread != NULL && WaitForSingleObject(writer_thread, 1000) != WAIT_TIMEOUT) {
            break;
        }
        if (reader_thread != NULL && WaitForSingleObject(reader_thread, 0) != WAIT_TIMEOUT) {
            break;
        }

        EnterCriticalSection(&context.lock);
        window_total = 0;
        for (i = 0; i < (int)SS_BER_WINDOW_SECONDS; ++i) {
            window_total += context.window_bytes[i];
        }
        average_bps = ((double)window_total * 8.0) / (double)SS_BER_WINDOW_SECONDS;
        error_rate = context.total_bits_checked > 0 ? (double)context.total_bit_errors / (double)context.total_bits_checked : 0.0;
        nothing_received_back = (context.total_bits_checked == previous_bits_checked) && (context.total_bits_sent > 0);
        previous_bits_checked = context.total_bits_checked;

        printf("Bits sent : %llu | Bit errors : %llu / %llu checked (%.2e) | Resyncs : %llu | Avg bitrate (%us window) : %.2f bps%s\n",
            (unsigned long long)context.total_bits_sent,
            (unsigned long long)context.total_bit_errors,
            (unsigned long long)context.total_bits_checked,
            error_rate,
            (unsigned long long)context.total_resyncs,
            SS_BER_WINDOW_SECONDS,
            average_bps,
            nothing_received_back ? " | WARNING: nothing received back -- link disconnected or receiver not echoing" : "");
        fflush(stdout);

        context.window_pos = (context.window_pos + 1) % (int)SS_BER_WINDOW_SECONDS;
        context.window_bytes[context.window_pos] = 0;
        LeaveCriticalSection(&context.lock);
    }

    InterlockedExchange(&context.stop_requested, 1);
    ss_transport_close(transport); /* unblocks any thread still blocked in send/recv */

    if (writer_thread != NULL) {
        WaitForSingleObject(writer_thread, INFINITE);
        CloseHandle(writer_thread);
    }
    if (reader_thread != NULL) {
        WaitForSingleObject(reader_thread, INFINITE);
        CloseHandle(reader_thread);
    }

    DeleteCriticalSection(&context.lock);
}

int main(int argc, char **argv)
{
    sender_config_t config;
    WSADATA wsa_data;
    ss_transport_t transport;
    ss_frame_t initial_frame;
    sender_region_queue_t region_queue;
    sender_send_queue_t send_queue;
    sender_capture_context_t capture_context;
    sender_encode_worker_context_t encode_worker_context;
    sender_send_context_t send_context;
    sender_debug_stats_t debug_stats;
    HANDLE input_thread;
    HANDLE capture_thread;
    HANDLE send_thread;
    HANDLE *encode_threads;
    DWORD encode_thread_count;
    HANDLE wait_handles[2];
    volatile LONG stop_requested = 0;
    int exit_code = 1;
    DWORD i;

    ZeroMemory(&transport, sizeof(transport));
    ZeroMemory(&initial_frame, sizeof(initial_frame));

    ss_win_set_process_dpi_aware(); /* otherwise GetSystemMetrics/BitBlt see a DPI-scaled-down desktop, not the real pixel resolution */

    {
        int parse_result = sender_parse_args(argc, argv, &config);
        if (parse_result != 0) {
            sender_print_usage();
            return parse_result > 0 ? 0 : 1;
        }
    }

    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        printf("WSAStartup failed.\n");
        return 1;
    }

    {
        HRESULT com_result = CoInitializeEx(NULL, COINIT_MULTITHREADED);
        if (com_result != S_OK && com_result != S_FALSE) {
            printf("COM initialization failed.\n");
            WSACleanup();
            return 1;
        }
    }

    if (config.transport_type == SS_TRANSPORT_UART) {
        if (ss_uart_transport_open(&transport, config.com_port, config.baud_rate) != 0) {
            printf("Unable to open COM%u at %lu baud\n", config.com_port, config.baud_rate);
            CoUninitialize();
            WSACleanup();
            return 1;
        }
    } else {
        if (ss_tcp_transport_connect(&transport, config.host, config.port) != 0) {
            printf("Unable to connect to %s:%u\n", config.host, config.port);
            CoUninitialize();
            WSACleanup();
            return 1;
        }
    }

    ss_jpeg_set_backend(config.jpeg_backend);
    ss_capture_set_backend(config.capture_backend);

    if (config.ber_mode) {
        sender_run_ber_mode(&transport);
        ss_transport_close(&transport);
        CoUninitialize();
        WSACleanup();
        return 0;
    }

    if (ss_capture_primary_screen(&initial_frame) != 0) {
        printf("Unable to capture the primary screen.\n");
        ss_transport_close(&transport);
        CoUninitialize();
        WSACleanup();
        return 1;
    }

    {
        SYSTEM_INFO system_info;
        GetSystemInfo(&system_info);
        encode_thread_count = system_info.dwNumberOfProcessors;
        if (encode_thread_count == 0) {
            encode_thread_count = 1;
        }
    }

    encode_threads = (HANDLE *)calloc(encode_thread_count, sizeof(HANDLE));
    if (encode_threads == NULL) {
        printf("Unable to allocate the encode worker thread table.\n");
        ss_frame_release(&initial_frame);
        ss_transport_close(&transport);
        CoUninitialize();
        WSACleanup();
        return 1;
    }

    sender_region_queue_init(&region_queue);
    sender_send_queue_init(&send_queue);

    {
        uint32_t columns = (initial_frame.width + config.tile_size - 1u) / config.tile_size;
        uint32_t rows = (initial_frame.height + config.tile_size - 1u) / config.tile_size;
        printf("Detected resolution : %ux%u\n", initial_frame.width, initial_frame.height);
        printf("Tile grid : %ux%u cells (%u total), tile size %u px\n", columns, rows, columns * rows, config.tile_size);
        printf("Encode workers : %u\n", (unsigned int)encode_thread_count);
    }

    if (sender_publish_full_frame(&region_queue, &initial_frame) != 0) {
        printf("Unable to queue the initial frame.\n");
        free(encode_threads);
        sender_region_queue_destroy(&region_queue);
        sender_send_queue_destroy(&send_queue);
        ss_frame_release(&initial_frame);
        ss_transport_close(&transport);
        CoUninitialize();
        WSACleanup();
        return 1;
    }

    input_thread = CreateThread(NULL, 0, sender_input_thread, &transport, 0, NULL);
    if (input_thread == NULL) {
        printf("Unable to create input thread.\n");
        free(encode_threads);
        sender_region_queue_destroy(&region_queue);
        sender_send_queue_destroy(&send_queue);
        ss_frame_release(&initial_frame);
        ss_transport_close(&transport);
        CoUninitialize();
        WSACleanup();
        return 1;
    }

    capture_context.config = config;
    capture_context.region_queue = &region_queue;
    capture_context.initial_frame = initial_frame; /* ownership moves to the capture thread */
    capture_context.stop_requested = &stop_requested;
    capture_context.debug_stats = NULL;

    encode_worker_context.region_queue = &region_queue;
    encode_worker_context.send_queue = &send_queue;
    encode_worker_context.debug_stats = NULL;

    send_context.transport = &transport;
    send_context.send_queue = &send_queue;
    send_context.stop_requested = &stop_requested;
    send_context.total_bytes_sent = 0;
    send_context.debug_stats = NULL;

    if (config.debug_enabled) {
        sender_debug_stats_init(&debug_stats);
        capture_context.debug_stats = &debug_stats;
        encode_worker_context.debug_stats = &debug_stats;
        send_context.debug_stats = &debug_stats;
    }

    capture_thread = CreateThread(NULL, 0, sender_capture_thread, &capture_context, 0, NULL);
    send_thread = CreateThread(NULL, 0, sender_send_thread, &send_context, 0, NULL);
    for (i = 0; i < encode_thread_count; ++i) {
        encode_threads[i] = CreateThread(NULL, 0, sender_encode_worker_thread, &encode_worker_context, 0, NULL);
    }

    {
        int startup_failed = (capture_thread == NULL || send_thread == NULL);

        for (i = 0; i < encode_thread_count; ++i) {
            if (encode_threads[i] == NULL) {
                startup_failed = 1;
            }
        }

        if (startup_failed) {
            printf("Unable to create capture/encode/send threads.\n");
            InterlockedExchange(&stop_requested, 1);
            sender_region_queue_signal_shutdown(&region_queue);
            sender_send_queue_signal_shutdown(&send_queue);
            ss_transport_close(&transport);

            if (capture_thread != NULL) {
                WaitForSingleObject(capture_thread, INFINITE);
                CloseHandle(capture_thread);
            } else {
                ss_frame_release(&capture_context.initial_frame);
            }

            for (i = 0; i < encode_thread_count; ++i) {
                if (encode_threads[i] != NULL) {
                    WaitForSingleObject(encode_threads[i], INFINITE);
                    CloseHandle(encode_threads[i]);
                }
            }

            if (send_thread != NULL) {
                WaitForSingleObject(send_thread, INFINITE);
                CloseHandle(send_thread);
            }

            WaitForSingleObject(input_thread, INFINITE);
            CloseHandle(input_thread);
            free(encode_threads);
            sender_region_queue_destroy(&region_queue);
            sender_send_queue_destroy(&send_queue);
            if (config.debug_enabled) {
                sender_debug_stats_destroy(&debug_stats);
            }
            CoUninitialize();
            WSACleanup();
            return 1;
        }
    }

    wait_handles[0] = capture_thread;
    wait_handles[1] = send_thread;
    WaitForMultipleObjects(2, wait_handles, FALSE, INFINITE);

    InterlockedExchange(&stop_requested, 1);
    sender_region_queue_signal_shutdown(&region_queue);
    sender_send_queue_signal_shutdown(&send_queue);

    WaitForSingleObject(capture_thread, INFINITE);
    CloseHandle(capture_thread);
    for (i = 0; i < encode_thread_count; ++i) {
        WaitForSingleObject(encode_threads[i], INFINITE);
        CloseHandle(encode_threads[i]);
    }
    WaitForSingleObject(send_thread, INFINITE);
    CloseHandle(send_thread);

    ss_transport_close(&transport);
    WaitForSingleObject(input_thread, INFINITE);
    CloseHandle(input_thread);

    free(encode_threads);
    sender_region_queue_destroy(&region_queue);
    sender_send_queue_destroy(&send_queue);
    if (config.debug_enabled) {
        sender_debug_stats_destroy(&debug_stats);
    }
    ss_capture_shutdown();
    ss_jpeg_shutdown();
    CoUninitialize();
    WSACleanup();

    return exit_code;
}

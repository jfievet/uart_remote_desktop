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
} sender_config_t;

static void sender_print_usage(void)
{
    printf("Usage: sender [--tcp | --uart] --host <ip> --port <port> [--com <n>] [--speed <baud>] [--interval-ms <ms>] [--tile-size <pixels>] [--ber]\n");
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

static int sender_send_message(ss_transport_t *transport, ss_message_type_t type, uint32_t x, uint32_t y, uint32_t width, uint32_t height, const uint8_t *pixels, uint32_t stride, uint64_t *bytes_sent)
{
    ss_message_header_t header;
    uint8_t *jpeg_data = NULL;
    size_t jpeg_size = 0;

    if (ss_jpeg_encode_bgra(pixels, width, height, stride, 0.80f, &jpeg_data, &jpeg_size) != 0) {
        return -1;
    }

    ss_protocol_make_header(&header, type, x, y, width, height, (uint32_t)jpeg_size);
    if (ss_transport_send_all(transport, &header, sizeof(header)) != 0 || ss_transport_send_all(transport, jpeg_data, jpeg_size) != 0) {
        free(jpeg_data);
        return -1;
    }

    *bytes_sent += sizeof(header) + jpeg_size;
    free(jpeg_data);
    return 0;
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

typedef struct sender_tile_slot {
    uint8_t *pixels;
    uint32_t width;
    uint32_t height;
} sender_tile_slot_t;

typedef struct sender_tile_queue {
    CRITICAL_SECTION lock;
    HANDLE ready_event;
    sender_tile_slot_t *tiles;
    uint32_t columns;
    uint32_t rows;
    uint32_t tile_size;
    sender_tile_slot_t full_frame;
} sender_tile_queue_t;

static int sender_tile_queue_configure(sender_tile_queue_t *queue, uint32_t frame_width, uint32_t frame_height, uint32_t tile_size)
{
    uint32_t columns = (frame_width + tile_size - 1u) / tile_size;
    uint32_t rows = (frame_height + tile_size - 1u) / tile_size;
    sender_tile_slot_t *tiles = (sender_tile_slot_t *)calloc((size_t)columns * (size_t)rows, sizeof(sender_tile_slot_t));

    if (tiles == NULL) {
        return -1;
    }

    EnterCriticalSection(&queue->lock);
    if (queue->tiles != NULL) {
        uint32_t index;
        for (index = 0; index < queue->columns * queue->rows; ++index) {
            free(queue->tiles[index].pixels);
        }
        free(queue->tiles);
    }
    free(queue->full_frame.pixels);
    ZeroMemory(&queue->full_frame, sizeof(queue->full_frame));
    queue->tiles = tiles;
    queue->columns = columns;
    queue->rows = rows;
    queue->tile_size = tile_size;
    LeaveCriticalSection(&queue->lock);

    return 0;
}

static int sender_tile_queue_create(sender_tile_queue_t *queue, uint32_t frame_width, uint32_t frame_height, uint32_t tile_size)
{
    ZeroMemory(queue, sizeof(*queue));
    InitializeCriticalSection(&queue->lock);
    queue->ready_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (queue->ready_event == NULL) {
        DeleteCriticalSection(&queue->lock);
        return -1;
    }

    if (sender_tile_queue_configure(queue, frame_width, frame_height, tile_size) != 0) {
        CloseHandle(queue->ready_event);
        DeleteCriticalSection(&queue->lock);
        return -1;
    }

    return 0;
}

static void sender_tile_queue_destroy(sender_tile_queue_t *queue)
{
    uint32_t index;

    EnterCriticalSection(&queue->lock);
    for (index = 0; index < queue->columns * queue->rows; ++index) {
        free(queue->tiles[index].pixels);
    }
    free(queue->tiles);
    free(queue->full_frame.pixels);
    LeaveCriticalSection(&queue->lock);

    CloseHandle(queue->ready_event);
    DeleteCriticalSection(&queue->lock);
}

static void sender_tile_queue_publish_tile(sender_tile_queue_t *queue, uint32_t column, uint32_t row, uint32_t width, uint32_t height, const uint8_t *pixels)
{
    size_t size = (size_t)width * (size_t)height * 4u;
    uint8_t *copy = (uint8_t *)malloc(size);

    if (copy == NULL) {
        return;
    }
    memcpy(copy, pixels, size);

    EnterCriticalSection(&queue->lock);
    if (column < queue->columns && row < queue->rows) {
        /* a still-unsent older snapshot for this cell is now stale -- drop it in favor of this newer one */
        sender_tile_slot_t *slot = &queue->tiles[(size_t)row * queue->columns + column];
        free(slot->pixels);
        slot->pixels = copy;
        slot->width = width;
        slot->height = height;
        copy = NULL;
    }
    LeaveCriticalSection(&queue->lock);

    free(copy);
    SetEvent(queue->ready_event);
}

static void sender_tile_queue_publish_full_frame(sender_tile_queue_t *queue, const ss_frame_t *frame)
{
    size_t size = (size_t)frame->stride * (size_t)frame->height;
    uint8_t *copy = (uint8_t *)malloc(size);

    if (copy == NULL) {
        return;
    }
    memcpy(copy, frame->pixels, size);

    EnterCriticalSection(&queue->lock);
    free(queue->full_frame.pixels);
    queue->full_frame.pixels = copy;
    queue->full_frame.width = frame->width;
    queue->full_frame.height = frame->height;
    LeaveCriticalSection(&queue->lock);

    SetEvent(queue->ready_event);
}

static uint8_t *sender_tile_queue_take_full_frame(sender_tile_queue_t *queue, uint32_t *width, uint32_t *height)
{
    uint8_t *pixels;

    EnterCriticalSection(&queue->lock);
    pixels = queue->full_frame.pixels;
    *width = queue->full_frame.width;
    *height = queue->full_frame.height;
    queue->full_frame.pixels = NULL;
    LeaveCriticalSection(&queue->lock);

    return pixels;
}

static uint8_t *sender_tile_queue_take_tile(sender_tile_queue_t *queue, uint32_t column, uint32_t row, uint32_t *width, uint32_t *height)
{
    uint8_t *pixels = NULL;

    EnterCriticalSection(&queue->lock);
    if (column < queue->columns && row < queue->rows) {
        sender_tile_slot_t *slot = &queue->tiles[(size_t)row * queue->columns + column];
        pixels = slot->pixels;
        *width = slot->width;
        *height = slot->height;
        slot->pixels = NULL;
    } else {
        *width = 0;
        *height = 0;
    }
    LeaveCriticalSection(&queue->lock);

    return pixels;
}

static void sender_publish_dirty_tiles(sender_tile_queue_t *queue, const ss_frame_t *current, const ss_frame_t *previous)
{
    uint32_t column;
    uint32_t row;
    uint32_t columns;
    uint32_t rows;
    uint32_t tile_size;

    EnterCriticalSection(&queue->lock);
    columns = queue->columns;
    rows = queue->rows;
    tile_size = queue->tile_size;
    LeaveCriticalSection(&queue->lock);

    for (row = 0; row < rows; ++row) {
        for (column = 0; column < columns; ++column) {
            uint32_t x = column * tile_size;
            uint32_t y = row * tile_size;
            uint32_t tile_width = current->width - x;
            uint32_t tile_height = current->height - y;
            uint8_t *tile_pixels;

            if (tile_width > tile_size) {
                tile_width = tile_size;
            }
            if (tile_height > tile_size) {
                tile_height = tile_size;
            }

            if (!sender_is_tile_dirty(current, previous, x, y, tile_width, tile_height)) {
                continue;
            }

            tile_pixels = (uint8_t *)malloc((size_t)tile_width * (size_t)tile_height * 4u);
            if (tile_pixels == NULL) {
                continue;
            }

            if (ss_frame_copy_region(current, x, y, tile_width, tile_height, tile_pixels, tile_width * 4u) == 0) {
                sender_tile_queue_publish_tile(queue, column, row, tile_width, tile_height, tile_pixels);
            }

            free(tile_pixels);
        }
    }
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
    sender_tile_queue_t *queue;
    ss_frame_t initial_frame; /* ownership transferred in from main; becomes the first "previous frame" */
    volatile LONG *stop_requested;
} sender_capture_context_t;

static DWORD WINAPI sender_capture_thread(LPVOID parameter)
{
    sender_capture_context_t *context = (sender_capture_context_t *)parameter;
    ss_frame_t previous_frame = context->initial_frame;

    while (*context->stop_requested == 0) {
        uint64_t cycle_start = ss_win_get_tick_count64();
        ss_frame_t current_frame;
        uint64_t cycle_elapsed;

        ZeroMemory(&current_frame, sizeof(current_frame));
        if (ss_capture_primary_screen(&current_frame) != 0) {
            InterlockedExchange(context->stop_requested, 1);
            break;
        }

        if (current_frame.width != previous_frame.width || current_frame.height != previous_frame.height) {
            if (sender_tile_queue_configure(context->queue, current_frame.width, current_frame.height, context->config.tile_size) != 0) {
                ss_frame_release(&current_frame);
                InterlockedExchange(context->stop_requested, 1);
                break;
            }
            sender_tile_queue_publish_full_frame(context->queue, &current_frame);
        } else {
            sender_publish_dirty_tiles(context->queue, &current_frame, &previous_frame);
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
    SetEvent(context->queue->ready_event); /* wake the network thread so it notices the stop request promptly */
    return 0;
}

typedef struct sender_network_context {
    ss_transport_t *transport;
    sender_tile_queue_t *queue;
    volatile LONG *stop_requested;
    uint64_t total_bytes_sent;
} sender_network_context_t;

static DWORD WINAPI sender_network_thread(LPVOID parameter)
{
    sender_network_context_t *context = (sender_network_context_t *)parameter;
    uint64_t start_tick = ss_win_get_tick_count64();
    uint64_t last_stat_tick = start_tick;
    uint64_t bytes_since_last_stat = 0;
    uint64_t cells_since_last_stat = 0;

    while (*context->stop_requested == 0) {
        uint64_t bytes_before = context->total_bytes_sent;
        uint32_t columns;
        uint32_t rows;
        uint32_t tile_size;
        uint32_t column;
        uint32_t row;
        uint32_t full_width;
        uint32_t full_height;
        uint8_t *full_frame_pixels;
        uint64_t now;

        WaitForSingleObject(context->queue->ready_event, 50);

        full_frame_pixels = sender_tile_queue_take_full_frame(context->queue, &full_width, &full_height);
        if (full_frame_pixels != NULL) {
            int failed = sender_send_message(context->transport, SS_MESSAGE_FULL_FRAME, 0, 0, full_width, full_height, full_frame_pixels, full_width * 4u, &context->total_bytes_sent) != 0;
            free(full_frame_pixels);
            if (failed) {
                InterlockedExchange(context->stop_requested, 1);
                break;
            }
        }

        EnterCriticalSection(&context->queue->lock);
        columns = context->queue->columns;
        rows = context->queue->rows;
        tile_size = context->queue->tile_size;
        LeaveCriticalSection(&context->queue->lock);

        for (row = 0; row < rows; ++row) {
            int stop = 0;
            for (column = 0; column < columns; ++column) {
                uint32_t tile_width;
                uint32_t tile_height;
                uint8_t *tile_pixels = sender_tile_queue_take_tile(context->queue, column, row, &tile_width, &tile_height);

                if (tile_pixels == NULL) {
                    continue;
                }

                if (sender_send_message(context->transport, SS_MESSAGE_UPDATE_REGION, column * tile_size, row * tile_size, tile_width, tile_height, tile_pixels, tile_width * 4u, &context->total_bytes_sent) != 0) {
                    free(tile_pixels);
                    stop = 1;
                    break;
                }
                free(tile_pixels);
                ++cells_since_last_stat;
            }
            if (stop) {
                InterlockedExchange(context->stop_requested, 1);
                break;
            }
        }

        now = ss_win_get_tick_count64();
        bytes_since_last_stat += context->total_bytes_sent - bytes_before;
        if (now - last_stat_tick >= 1000u) {
            sender_print_stats(context->total_bytes_sent, start_tick, last_stat_tick, bytes_since_last_stat, cells_since_last_stat);
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
    sender_tile_queue_t queue;
    sender_capture_context_t capture_context;
    sender_network_context_t network_context;
    HANDLE input_thread;
    HANDLE capture_thread;
    HANDLE network_thread;
    HANDLE wait_handles[2];
    volatile LONG stop_requested = 0;
    int exit_code = 1;

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

    if (sender_tile_queue_create(&queue, initial_frame.width, initial_frame.height, config.tile_size) != 0) {
        printf("Unable to allocate the tile queue.\n");
        ss_frame_release(&initial_frame);
        ss_transport_close(&transport);
        CoUninitialize();
        WSACleanup();
        return 1;
    }

    printf("Detected resolution : %ux%u\n", initial_frame.width, initial_frame.height);
    printf("Tile grid : %ux%u cells (%u total), tile size %u px\n", queue.columns, queue.rows, queue.columns * queue.rows, config.tile_size);

    sender_tile_queue_publish_full_frame(&queue, &initial_frame);

    input_thread = CreateThread(NULL, 0, sender_input_thread, &transport, 0, NULL);
    if (input_thread == NULL) {
        printf("Unable to create input thread.\n");
        ss_frame_release(&initial_frame);
        sender_tile_queue_destroy(&queue);
        ss_transport_close(&transport);
        CoUninitialize();
        WSACleanup();
        return 1;
    }

    capture_context.config = config;
    capture_context.queue = &queue;
    capture_context.initial_frame = initial_frame; /* ownership moves to the capture thread */
    capture_context.stop_requested = &stop_requested;

    network_context.transport = &transport;
    network_context.queue = &queue;
    network_context.stop_requested = &stop_requested;
    network_context.total_bytes_sent = 0;

    capture_thread = CreateThread(NULL, 0, sender_capture_thread, &capture_context, 0, NULL);
    network_thread = CreateThread(NULL, 0, sender_network_thread, &network_context, 0, NULL);

    if (capture_thread == NULL || network_thread == NULL) {
        printf("Unable to create capture/network threads.\n");
        InterlockedExchange(&stop_requested, 1);
        SetEvent(queue.ready_event);
        ss_transport_close(&transport);

        if (capture_thread != NULL) {
            WaitForSingleObject(capture_thread, INFINITE);
            CloseHandle(capture_thread);
        } else {
            ss_frame_release(&capture_context.initial_frame);
        }

        if (network_thread != NULL) {
            WaitForSingleObject(network_thread, INFINITE);
            CloseHandle(network_thread);
        }

        WaitForSingleObject(input_thread, INFINITE);
        CloseHandle(input_thread);
        sender_tile_queue_destroy(&queue);
        CoUninitialize();
        WSACleanup();
        return 1;
    }

    wait_handles[0] = capture_thread;
    wait_handles[1] = network_thread;
    WaitForMultipleObjects(2, wait_handles, FALSE, INFINITE);

    InterlockedExchange(&stop_requested, 1);
    SetEvent(queue.ready_event);

    WaitForSingleObject(capture_thread, INFINITE);
    WaitForSingleObject(network_thread, INFINITE);
    CloseHandle(capture_thread);
    CloseHandle(network_thread);

    ss_transport_close(&transport);
    WaitForSingleObject(input_thread, INFINITE);
    CloseHandle(input_thread);

    sender_tile_queue_destroy(&queue);
    ss_jpeg_shutdown();
    CoUninitialize();
    WSACleanup();

    return exit_code;
}

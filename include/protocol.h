#ifndef SCREEN_SHARING_PROTOCOL_H
#define SCREEN_SHARING_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define SS_PROTOCOL_MAGIC 0x53534852u
#define SS_PROTOCOL_VERSION 1u

typedef enum ss_message_type {
    SS_MESSAGE_FULL_FRAME = 1u,
    SS_MESSAGE_UPDATE_REGION = 2u,
    SS_MESSAGE_MOUSE_EVENT = 3u,
    SS_MESSAGE_KEY_EVENT = 4u
} ss_message_type_t;

typedef enum ss_mouse_action {
    SS_MOUSE_ACTION_MOVE = 0u,
    SS_MOUSE_ACTION_BUTTON_DOWN = 1u,
    SS_MOUSE_ACTION_BUTTON_UP = 2u,
    SS_MOUSE_ACTION_WHEEL = 3u
} ss_mouse_action_t;

typedef enum ss_mouse_button {
    SS_MOUSE_BUTTON_NONE = 0u,
    SS_MOUSE_BUTTON_LEFT = 1u,
    SS_MOUSE_BUTTON_RIGHT = 2u,
    SS_MOUSE_BUTTON_MIDDLE = 3u
} ss_mouse_button_t;

typedef enum ss_key_action {
    SS_KEY_ACTION_DOWN = 0u,
    SS_KEY_ACTION_UP = 1u
} ss_key_action_t;

#pragma pack(push, 1)
typedef struct ss_message_header {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t payload_size;
} ss_message_header_t;

typedef struct ss_mouse_event {
    uint32_t x;
    uint32_t y;
    uint32_t action;
    int32_t param; /* button id for down/up, wheel delta for wheel, unused for move */
} ss_mouse_event_t;

typedef struct ss_key_event {
    uint32_t vk_code;
    uint32_t scan_code;
    uint32_t action;
    uint32_t extended;
} ss_key_event_t;
#pragma pack(pop)

void ss_protocol_make_header(ss_message_header_t *header, ss_message_type_t type, uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t payload_size);
int ss_protocol_validate_header(const ss_message_header_t *header);
size_t ss_protocol_header_size(void);
void ss_protocol_encode_mouse_event(ss_mouse_event_t *out, uint32_t x, uint32_t y, ss_mouse_action_t action, int32_t param);
void ss_protocol_decode_mouse_event(const ss_mouse_event_t *in, uint32_t *x, uint32_t *y, ss_mouse_action_t *action, int32_t *param);
void ss_protocol_encode_key_event(ss_key_event_t *out, uint32_t vk_code, uint32_t scan_code, ss_key_action_t action, uint32_t extended);
void ss_protocol_decode_key_event(const ss_key_event_t *in, uint32_t *vk_code, uint32_t *scan_code, ss_key_action_t *action, uint32_t *extended);

#endif

#include "protocol.h"

#include <winsock2.h>

void ss_protocol_make_header(ss_message_header_t *header, ss_message_type_t type, uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t payload_size)
{
    header->magic = htonl(SS_PROTOCOL_MAGIC);
    header->version = htons((uint16_t)SS_PROTOCOL_VERSION);
    header->type = htons((uint16_t)type);
    header->x = htonl(x);
    header->y = htonl(y);
    header->width = htonl(width);
    header->height = htonl(height);
    header->payload_size = htonl(payload_size);
}

int ss_protocol_validate_header(const ss_message_header_t *header)
{
    if (ntohl(header->magic) != SS_PROTOCOL_MAGIC) {
        return 0;
    }

    if (ntohs(header->version) != SS_PROTOCOL_VERSION) {
        return 0;
    }

    if (ntohs(header->type) != SS_MESSAGE_FULL_FRAME && ntohs(header->type) != SS_MESSAGE_UPDATE_REGION && ntohs(header->type) != SS_MESSAGE_MOUSE_EVENT && ntohs(header->type) != SS_MESSAGE_KEY_EVENT) {
        return 0;
    }

    return 1;
}

size_t ss_protocol_header_size(void)
{
    return sizeof(ss_message_header_t);
}

void ss_protocol_encode_mouse_event(ss_mouse_event_t *out, uint32_t x, uint32_t y, ss_mouse_action_t action, int32_t param)
{
    out->x = htonl(x);
    out->y = htonl(y);
    out->action = htonl((uint32_t)action);
    out->param = (int32_t)htonl((uint32_t)param);
}

void ss_protocol_decode_mouse_event(const ss_mouse_event_t *in, uint32_t *x, uint32_t *y, ss_mouse_action_t *action, int32_t *param)
{
    *x = ntohl(in->x);
    *y = ntohl(in->y);
    *action = (ss_mouse_action_t)ntohl(in->action);
    *param = (int32_t)ntohl((uint32_t)in->param);
}

void ss_protocol_encode_key_event(ss_key_event_t *out, uint32_t vk_code, uint32_t scan_code, ss_key_action_t action, uint32_t extended)
{
    out->vk_code = htonl(vk_code);
    out->scan_code = htonl(scan_code);
    out->action = htonl((uint32_t)action);
    out->extended = htonl(extended);
}

void ss_protocol_decode_key_event(const ss_key_event_t *in, uint32_t *vk_code, uint32_t *scan_code, ss_key_action_t *action, uint32_t *extended)
{
    *vk_code = ntohl(in->vk_code);
    *scan_code = ntohl(in->scan_code);
    *action = (ss_key_action_t)ntohl(in->action);
    *extended = ntohl(in->extended);
}

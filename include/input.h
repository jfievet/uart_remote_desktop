#ifndef SCREEN_SHARING_INPUT_H
#define SCREEN_SHARING_INPUT_H

#include "protocol.h"

int ss_input_inject_mouse_event(uint32_t x, uint32_t y, ss_mouse_action_t action, int32_t param);
int ss_input_inject_key_event(uint32_t vk_code, uint32_t scan_code, ss_key_action_t action, uint32_t extended);

#endif

#define WIN32_LEAN_AND_MEAN

#include "input.h"

#include <windows.h>

static DWORD ss_input_button_down_flag(ss_mouse_button_t button)
{
    switch (button) {
    case SS_MOUSE_BUTTON_LEFT:
        return MOUSEEVENTF_LEFTDOWN;
    case SS_MOUSE_BUTTON_RIGHT:
        return MOUSEEVENTF_RIGHTDOWN;
    case SS_MOUSE_BUTTON_MIDDLE:
        return MOUSEEVENTF_MIDDLEDOWN;
    default:
        return 0;
    }
}

static DWORD ss_input_button_up_flag(ss_mouse_button_t button)
{
    switch (button) {
    case SS_MOUSE_BUTTON_LEFT:
        return MOUSEEVENTF_LEFTUP;
    case SS_MOUSE_BUTTON_RIGHT:
        return MOUSEEVENTF_RIGHTUP;
    case SS_MOUSE_BUTTON_MIDDLE:
        return MOUSEEVENTF_MIDDLEUP;
    default:
        return 0;
    }
}

int ss_input_inject_mouse_event(uint32_t x, uint32_t y, ss_mouse_action_t action, int32_t param)
{
    INPUT input;
    int screen_width = GetSystemMetrics(SM_CXSCREEN);
    int screen_height = GetSystemMetrics(SM_CYSCREEN);

    if (screen_width <= 1 || screen_height <= 1) {
        return -1;
    }

    ZeroMemory(&input, sizeof(input));
    input.type = INPUT_MOUSE;
    /* SendInput absolute coordinates are normalized to the 0..65535 range regardless of screen resolution */
    input.mi.dx = (LONG)(((LONGLONG)x * 65535) / (screen_width - 1));
    input.mi.dy = (LONG)(((LONGLONG)y * 65535) / (screen_height - 1));
    input.mi.dwFlags = MOUSEEVENTF_ABSOLUTE;

    switch (action) {
    case SS_MOUSE_ACTION_MOVE:
        input.mi.dwFlags |= MOUSEEVENTF_MOVE;
        break;
    case SS_MOUSE_ACTION_BUTTON_DOWN:
        input.mi.dwFlags |= ss_input_button_down_flag((ss_mouse_button_t)param);
        break;
    case SS_MOUSE_ACTION_BUTTON_UP:
        input.mi.dwFlags |= ss_input_button_up_flag((ss_mouse_button_t)param);
        break;
    case SS_MOUSE_ACTION_WHEEL:
        input.mi.dwFlags |= MOUSEEVENTF_WHEEL;
        input.mi.mouseData = (DWORD)(param * WHEEL_DELTA);
        break;
    default:
        return -1;
    }

    return SendInput(1, &input, sizeof(INPUT)) == 1 ? 0 : -1;
}

int ss_input_inject_key_event(uint32_t vk_code, uint32_t scan_code, ss_key_action_t action, uint32_t extended)
{
    INPUT input;

    ZeroMemory(&input, sizeof(input));
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = (WORD)vk_code;
    input.ki.wScan = (WORD)scan_code;
    input.ki.dwFlags = (extended ? KEYEVENTF_EXTENDEDKEY : 0) | (action == SS_KEY_ACTION_UP ? KEYEVENTF_KEYUP : 0);

    return SendInput(1, &input, sizeof(INPUT)) == 1 ? 0 : -1;
}

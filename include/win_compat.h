#ifndef SCREEN_SHARING_WIN_COMPAT_H
#define SCREEN_SHARING_WIN_COMPAT_H

#include <stdint.h>
#include <windows.h>

static uint64_t ss_win_get_tick_count64(void)
{
    typedef ULONGLONG (WINAPI *ss_get_tick_count64_fn)(void);
    static ss_get_tick_count64_fn tick_count64 = NULL;
    static int looked_up = 0;

    if (!looked_up) {
        HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
        if (kernel32 != NULL) {
            tick_count64 = (ss_get_tick_count64_fn)GetProcAddress(kernel32, "GetTickCount64");
        }
        looked_up = 1;
    }

    if (tick_count64 != NULL) {
        return (uint64_t)tick_count64();
    }

    return (uint64_t)GetTickCount();
}

static uint64_t ss_win_get_perf_counter(void)
{
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (uint64_t)counter.QuadPart;
}

/* elapsed time in milliseconds between two ss_win_get_perf_counter() samples */
static double ss_win_perf_counter_to_ms(uint64_t start, uint64_t end)
{
    static LARGE_INTEGER frequency;
    static int looked_up = 0;

    if (!looked_up) {
        QueryPerformanceFrequency(&frequency);
        looked_up = 1;
    }

    return frequency.QuadPart > 0 ? ((double)(end - start) * 1000.0) / (double)frequency.QuadPart : 0.0;
}

static void ss_win_set_process_dpi_aware(void)
{
    typedef BOOL (WINAPI *ss_set_process_dpi_aware_fn)(void);
    HMODULE user32 = GetModuleHandleA("user32.dll");
    ss_set_process_dpi_aware_fn set_process_dpi_aware;

    if (user32 == NULL) {
        return;
    }

    set_process_dpi_aware = (ss_set_process_dpi_aware_fn)GetProcAddress(user32, "SetProcessDPIAware");
    if (set_process_dpi_aware != NULL) {
        set_process_dpi_aware();
    }
}

#endif

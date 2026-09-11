#define WIN32_LEAN_AND_MEAN

#include "transport.h"

#include <windows.h>
#include <stdio.h>

int ss_uart_transport_open(ss_transport_t *transport, unsigned int com_port, unsigned long baud_rate)
{
    char device_path[32];
    HANDLE handle;
    HANDLE read_event;
    HANDLE write_event;
    DCB dcb;
    COMMTIMEOUTS timeouts;

    snprintf(device_path, sizeof(device_path), "\\\\.\\COM%u", com_port);

    /* overlapped mode is required here: the writer and reader threads issue WriteFile/ReadFile
       concurrently on this same handle, which a synchronous (non-overlapped) handle cannot support */
    handle = CreateFileA(device_path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        return -1;
    }

    SetupComm(handle, 65536, 65536); /* larger driver buffers reduce receive-overrun byte loss at high baud rates; best-effort, not fatal if unsupported */

    ZeroMemory(&dcb, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(handle, &dcb)) {
        CloseHandle(handle);
        return -1;
    }

    dcb.BaudRate = baud_rate;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;

    if (!SetCommState(handle, &dcb)) {
        CloseHandle(handle);
        return -1;
    }

    /* all-zero timeouts make ReadFile/WriteFile block until the requested byte count completes, matching send_all/recv_exact semantics */
    ZeroMemory(&timeouts, sizeof(timeouts));
    if (!SetCommTimeouts(handle, &timeouts)) {
        CloseHandle(handle);
        return -1;
    }

    read_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    write_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (read_event == NULL || write_event == NULL) {
        if (read_event != NULL) {
            CloseHandle(read_event);
        }
        if (write_event != NULL) {
            CloseHandle(write_event);
        }
        CloseHandle(handle);
        return -1;
    }

    transport->type = SS_TRANSPORT_UART;
    transport->serial_handle = handle;
    transport->serial_read_event = read_event;
    transport->serial_write_event = write_event;
    transport->socket_handle = INVALID_SOCKET;

    return 0;
}

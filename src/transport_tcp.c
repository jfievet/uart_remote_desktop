#define WIN32_LEAN_AND_MEAN

#include "transport.h"

#include <stdio.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winerror.h>
#include <stdlib.h>

static int ss_transport_store_socket(ss_transport_t *transport, SOCKET socket_handle)
{
    int nodelay_enable = 1;
    int buffer_size = 1 * 1024 * 1024;

    /* disable Nagle -- without it, small header/payload writes can stall waiting for an ACK */
    setsockopt(socket_handle, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay_enable, sizeof(nodelay_enable));

    /* larger kernel buffers avoid throttling bursts of many encoded messages at high throughput */
    setsockopt(socket_handle, SOL_SOCKET, SO_SNDBUF, (const char *)&buffer_size, sizeof(buffer_size));
    setsockopt(socket_handle, SOL_SOCKET, SO_RCVBUF, (const char *)&buffer_size, sizeof(buffer_size));

    transport->type = SS_TRANSPORT_TCP;
    transport->socket_handle = socket_handle;
    transport->serial_handle = INVALID_HANDLE_VALUE;
    transport->serial_read_event = NULL;
    transport->serial_write_event = NULL;
    return 0;
}

static int ss_transport_connect_socket(const char *host, unsigned short port, SOCKET *out_socket)
{
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    char port_text[16];
    int status;

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    snprintf(port_text, sizeof(port_text), "%u", port);
    status = getaddrinfo(host, port_text, &hints, &result);
    if (status != 0) {
        return -1;
    }

    *out_socket = INVALID_SOCKET;

    {
        struct addrinfo *current;
        for (current = result; current != NULL; current = current->ai_next) {
            SOCKET socket_handle = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
            if (socket_handle == INVALID_SOCKET) {
                continue;
            }

            if (connect(socket_handle, current->ai_addr, (int)current->ai_addrlen) == 0) {
                *out_socket = socket_handle;
                break;
            }

            closesocket(socket_handle);
        }
    }

    freeaddrinfo(result);
    return *out_socket == INVALID_SOCKET ? -1 : 0;
}

int ss_tcp_transport_connect(ss_transport_t *transport, const char *host, unsigned short port)
{
    SOCKET socket_handle;

    if (ss_transport_connect_socket(host, port, &socket_handle) != 0) {
        return -1;
    }

    return ss_transport_store_socket(transport, socket_handle);
}

int ss_tcp_transport_accept(ss_transport_t *transport, unsigned short port)
{
    SOCKET listen_socket = INVALID_SOCKET;
    SOCKET client_socket = INVALID_SOCKET;
    struct sockaddr_in address;
    int reuse_address = 1;

    listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_socket == INVALID_SOCKET) {
        return -1;
    }

    setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse_address, sizeof(reuse_address));

    ZeroMemory(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    if (bind(listen_socket, (struct sockaddr *)&address, sizeof(address)) != 0) {
        closesocket(listen_socket);
        return -1;
    }

    if (listen(listen_socket, 1) != 0) {
        closesocket(listen_socket);
        return -1;
    }

    client_socket = accept(listen_socket, NULL, NULL);
    closesocket(listen_socket);
    if (client_socket == INVALID_SOCKET) {
        return -1;
    }

    return ss_transport_store_socket(transport, client_socket);
}

int ss_transport_send_all(ss_transport_t *transport, const void *data, size_t size)
{
    const char *buffer = (const char *)data;
    size_t remaining = size;

    if (transport->type == SS_TRANSPORT_UART) {
        while (remaining > 0) {
            OVERLAPPED overlapped;
            DWORD written = 0;

            ZeroMemory(&overlapped, sizeof(overlapped));
            overlapped.hEvent = transport->serial_write_event;
            ResetEvent(transport->serial_write_event);

            if (!WriteFile(transport->serial_handle, buffer, (DWORD)remaining, &written, &overlapped)) {
                if (GetLastError() != ERROR_IO_PENDING) {
                    return -1;
                }
                if (!GetOverlappedResult(transport->serial_handle, &overlapped, &written, TRUE)) {
                    return -1;
                }
            }

            if (written == 0) {
                return -1;
            }

            buffer += written;
            remaining -= (size_t)written;
        }
        return 0;
    }

    while (remaining > 0) {
        int sent = send(transport->socket_handle, buffer, (int)remaining, 0);
        if (sent <= 0) {
            return -1;
        }

        buffer += sent;
        remaining -= (size_t)sent;
    }

    return 0;
}

int ss_transport_recv_exact(ss_transport_t *transport, void *data, size_t size)
{
    char *buffer = (char *)data;
    size_t remaining = size;

    if (transport->type == SS_TRANSPORT_UART) {
        while (remaining > 0) {
            OVERLAPPED overlapped;
            DWORD received = 0;

            ZeroMemory(&overlapped, sizeof(overlapped));
            overlapped.hEvent = transport->serial_read_event;
            ResetEvent(transport->serial_read_event);

            if (!ReadFile(transport->serial_handle, buffer, (DWORD)remaining, &received, &overlapped)) {
                if (GetLastError() != ERROR_IO_PENDING) {
                    return -1;
                }
                if (!GetOverlappedResult(transport->serial_handle, &overlapped, &received, TRUE)) {
                    return -1;
                }
            }

            if (received == 0) {
                return -1;
            }

            buffer += received;
            remaining -= (size_t)received;
        }
        return 0;
    }

    while (remaining > 0) {
        int received = recv(transport->socket_handle, buffer, (int)remaining, 0);
        if (received <= 0) {
            return -1;
        }

        buffer += received;
        remaining -= (size_t)received;
    }

    return 0;
}

void ss_transport_close(ss_transport_t *transport)
{
    if (transport == NULL) {
        return;
    }

    if (transport->type == SS_TRANSPORT_UART) {
        if (transport->serial_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(transport->serial_handle);
            transport->serial_handle = INVALID_HANDLE_VALUE;
        }
        if (transport->serial_read_event != NULL) {
            CloseHandle(transport->serial_read_event);
            transport->serial_read_event = NULL;
        }
        if (transport->serial_write_event != NULL) {
            CloseHandle(transport->serial_write_event);
            transport->serial_write_event = NULL;
        }
        return;
    }

    if (transport->socket_handle != INVALID_SOCKET) {
        closesocket(transport->socket_handle);
        transport->socket_handle = INVALID_SOCKET;
    }
}

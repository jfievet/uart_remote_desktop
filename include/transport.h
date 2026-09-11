#ifndef SCREEN_SHARING_TRANSPORT_H
#define SCREEN_SHARING_TRANSPORT_H

#include <stddef.h>
#include <winsock2.h>
#include <windows.h>

typedef enum ss_transport_type {
    SS_TRANSPORT_TCP = 0,
    SS_TRANSPORT_UART = 1
} ss_transport_type_t;

typedef struct ss_transport {
	ss_transport_type_t type;
	SOCKET socket_handle;
	HANDLE serial_handle;
	HANDLE serial_read_event;
	HANDLE serial_write_event;
} ss_transport_t;

int ss_tcp_transport_connect(ss_transport_t *transport, const char *host, unsigned short port);
int ss_tcp_transport_accept(ss_transport_t *transport, unsigned short port);
int ss_uart_transport_open(ss_transport_t *transport, unsigned int com_port, unsigned long baud_rate);
int ss_transport_send_all(ss_transport_t *transport, const void *data, size_t size);
int ss_transport_recv_exact(ss_transport_t *transport, void *data, size_t size);
void ss_transport_close(ss_transport_t *transport);

#endif

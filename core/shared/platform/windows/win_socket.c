/*
 * Copyright (C) 2021 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "platform_api_vmcore.h"
#include "platform_api_extension.h"
#include "platform_wasi_types.h"
#include "win_util.h"

/* link with Ws2_32.lib */
#pragma comment(lib, "ws2_32.lib")

static bool is_winsock_inited = false;

/* The WASI socket wrappers translate errno, not WSAGetLastError(). */
static int
socket_error(void)
{
    switch (WSAGetLastError()) {
        case WSAEWOULDBLOCK: errno = EAGAIN; break;
        case WSAEINPROGRESS: errno = EINPROGRESS; break;
        case WSAEALREADY: errno = EALREADY; break;
        case WSAECONNREFUSED: errno = ECONNREFUSED; break;
        case WSAECONNRESET: errno = ECONNRESET; break;
        case WSAECONNABORTED: errno = ECONNABORTED; break;
        case WSAENOTCONN: errno = ENOTCONN; break;
        case WSAEADDRINUSE: errno = EADDRINUSE; break;
        case WSAEADDRNOTAVAIL: errno = EADDRNOTAVAIL; break;
        case WSAETIMEDOUT: errno = ETIMEDOUT; break;
        case WSAENETUNREACH: errno = ENETUNREACH; break;
        case WSAEHOSTUNREACH: errno = EHOSTUNREACH; break;
        case WSAEACCES: errno = EACCES; break;
        case WSAEINTR: errno = EINTR; break;
        case WSAENOTSOCK: errno = ENOTSOCK; break;
        default: errno = EINVAL; break;
    }
    return BHT_ERROR;
}

static int
socket_address(const struct sockaddr_in *addr, bh_sockaddr_t *out)
{
    if (addr->sin_family != AF_INET) {
        errno = EAFNOSUPPORT;
        return BHT_ERROR;
    }
    out->is_ipv4 = true;
    out->port = ntohs(addr->sin_port);
    out->addr_buffer.ipv4 = ntohl(addr->sin_addr.s_addr);
    return BHT_OK;
}

static int
socket_option(bh_socket_t socket, int level, int option, int value)
{
    if (setsockopt(socket->raw.socket, level, option, (const char *)&value,
                   sizeof(value)) == SOCKET_ERROR)
        return socket_error();
    return BHT_OK;
}

#define CHECK_VALID_SOCKET_HANDLE(win_handle)                   \
    do {                                                        \
        if ((win_handle) == NULL) {                             \
            errno = EBADF;                                      \
            return BHT_ERROR;                                   \
        }                                                       \
        if ((win_handle)->type != windows_handle_type_socket) { \
            errno = ENOTSOCK;                                   \
            return BHT_ERROR;                                   \
        }                                                       \
        if ((win_handle)->raw.socket == INVALID_SOCKET) {       \
            errno = EBADF;                                      \
            return BHT_ERROR;                                   \
        }                                                       \
    } while (0)

int
init_winsock()
{
#if WASM_ENABLE_HOST_SOCKET_INIT == 0
    WSADATA wsaData;

    if (!is_winsock_inited) {
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            os_printf("winsock init failed");
            return BHT_ERROR;
        }

        is_winsock_inited = true;
    }
#endif

    return BHT_OK;
}

void
deinit_winsock()
{
#if WASM_ENABLE_HOST_SOCKET_INIT == 0
    if (is_winsock_inited) {
        WSACleanup();
    }
#endif
}

int
os_socket_create(bh_socket_t *sock, bool is_ipv4, bool is_tcp)
{
    int af;

    if (!sock) {
        return BHT_ERROR;
    }

    *(sock) = BH_MALLOC(sizeof(windows_handle));

    if ((*sock) == NULL) {
        errno = ENOMEM;
        return BHT_ERROR;
    }

    (*sock)->type = windows_handle_type_socket;
    (*sock)->access_mode = windows_access_mode_read | windows_access_mode_write;
    (*sock)->fdflags = 0;

    if (is_ipv4) {
        af = AF_INET;
    }
    else {
        BH_FREE(*sock);
        *sock = NULL;
        errno = ENOSYS;
        return BHT_ERROR;
    }

    if (is_tcp) {
        (*sock)->raw.socket = socket(af, SOCK_STREAM, IPPROTO_TCP);
    }
    else {
        (*sock)->raw.socket = socket(af, SOCK_DGRAM, 0);
    }

    if ((*sock)->raw.socket == INVALID_SOCKET) {
        int error = WSAGetLastError();
        BH_FREE(*sock);
        *sock = NULL;
        WSASetLastError(error);
        return socket_error();
    }

    return BHT_OK;
}

int
os_socket_bind(bh_socket_t socket, const char *host, int *port)
{
    CHECK_VALID_SOCKET_HANDLE(socket);
    struct sockaddr_in addr;
    int socklen, ret;

    assert(host);
    assert(port);

    addr.sin_addr.s_addr = inet_addr(host);
    addr.sin_port = htons(*port);
    addr.sin_family = AF_INET;

    ret = bind(socket->raw.socket, (struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0) {
        goto fail;
    }

    socklen = sizeof(addr);
    if (getsockname(socket->raw.socket, (void *)&addr, &socklen) == -1) {
        os_printf("getsockname failed with error %d\n", WSAGetLastError());
        goto fail;
    }

    *port = ntohs(addr.sin_port);

    return BHT_OK;

fail:
    return socket_error();
}

int
os_socket_settimeout(bh_socket_t socket, uint64 timeout_us)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    DWORD tv = (DWORD)(timeout_us / 1000UL);

    if (setsockopt(socket->raw.socket, SOL_SOCKET, SO_RCVTIMEO,
                   (const char *)&tv, sizeof(tv))
        != 0) {
        return socket_error();
    }

    return BHT_OK;
}

int
os_socket_listen(bh_socket_t socket, int max_client)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    if (listen(socket->raw.socket, max_client) != 0) {
        return socket_error();
    }

    return BHT_OK;
}

int
os_socket_accept(bh_socket_t server_sock, bh_socket_t *sock, void *addr,
                 unsigned int *addrlen)
{
    CHECK_VALID_SOCKET_HANDLE(server_sock);

    struct sockaddr addr_tmp;
    unsigned int len = sizeof(struct sockaddr);

    *sock = BH_MALLOC(sizeof(windows_handle));

    if (*sock == NULL) {
        errno = ENOMEM;
        return BHT_ERROR;
    }

    (*sock)->type = windows_handle_type_socket;
    (*sock)->access_mode = windows_access_mode_read | windows_access_mode_write;
    (*sock)->fdflags = 0;
    (*sock)->raw.socket = accept(server_sock->raw.socket,
                                 (struct sockaddr *)&addr_tmp, (int *)&len);

    if ((*sock)->raw.socket == INVALID_SOCKET) {
        int error = WSAGetLastError();
        BH_FREE(*sock);
        *sock = NULL;
        WSASetLastError(error);
        return socket_error();
    }

    return BHT_OK;
}

int
os_socket_recv(bh_socket_t socket, void *buf, unsigned int len)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    int ret = recv(socket->raw.socket, buf, len, 0);
    return ret == SOCKET_ERROR ? socket_error() : ret;
}

int
os_socket_recv_from(bh_socket_t socket, void *buf, unsigned int len, int flags,
                    bh_sockaddr_t *src_addr)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    struct sockaddr_in addr = { 0 };
    int addr_len = sizeof(addr);
    int ret = recvfrom(socket->raw.socket, buf, len, flags,
                       src_addr ? (struct sockaddr *)&addr : NULL,
                       src_addr ? &addr_len : NULL);
    if (ret == SOCKET_ERROR)
        return socket_error();
    if (src_addr && socket_address(&addr, src_addr) != BHT_OK)
        return BHT_ERROR;
    return ret;
}

int
os_socket_send(bh_socket_t socket, const void *buf, unsigned int len)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    int ret = send(socket->raw.socket, buf, len, 0);
    return ret == SOCKET_ERROR ? socket_error() : ret;
}

int
os_socket_send_to(bh_socket_t socket, const void *buf, unsigned int len,
                  int flags, const bh_sockaddr_t *dest_addr)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    if (!dest_addr || !dest_addr->is_ipv4) {
        errno = EAFNOSUPPORT;
        return BHT_ERROR;
    }
    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_port = htons(dest_addr->port);
    addr.sin_addr.s_addr = htonl(dest_addr->addr_buffer.ipv4);
    int ret = sendto(socket->raw.socket, buf, len, flags,
                     (struct sockaddr *)&addr, sizeof(addr));
    return ret == SOCKET_ERROR ? socket_error() : ret;
}

int
os_socket_close(bh_socket_t socket)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    closesocket(socket->raw.socket);

    BH_FREE(socket);

    return BHT_OK;
}

__wasi_errno_t
os_socket_shutdown(bh_socket_t socket)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    if (shutdown(socket->raw.socket, SD_BOTH) != 0) {
        return convert_winsock_error_code(WSAGetLastError());
    }
    return __WASI_ESUCCESS;
}

int
os_socket_inet_network(bool is_ipv4, const char *cp, bh_ip_addr_buffer_t *out)
{
    if (!cp)
        return BHT_ERROR;

    if (is_ipv4) {
        if (inet_pton(AF_INET, cp, &out->ipv4) != 1) {
            return BHT_ERROR;
        }
        /* Note: ntohl(INADDR_NONE) == INADDR_NONE */
        out->ipv4 = ntohl(out->ipv4);
    }
    else {
        if (inet_pton(AF_INET6, cp, out->ipv6) != 1) {
            return BHT_ERROR;
        }
        for (int i = 0; i < 8; i++) {
            out->ipv6[i] = ntohs(out->ipv6[i]);
        }
    }

    return BHT_OK;
}

int
os_socket_connect(bh_socket_t socket, const char *addr, int port)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    struct sockaddr_in target = { 0 };
    target.sin_family = AF_INET;
    target.sin_port = htons(port);
    if (inet_pton(AF_INET, addr, &target.sin_addr) != 1) {
        errno = EINVAL;
        return BHT_ERROR;
    }
    if (connect(socket->raw.socket, (struct sockaddr *)&target, sizeof(target))
        == SOCKET_ERROR) {
        if (WSAGetLastError() == WSAEWOULDBLOCK) {
            errno = EINPROGRESS;
            return BHT_ERROR;
        }
        return socket_error();
    }
    return BHT_OK;
}

int
os_socket_addr_resolve(const char *host, const char *service,
                       uint8_t *hint_is_tcp, uint8_t *hint_is_ipv4,
                       bh_addr_info_t *addr_info, size_t addr_info_size,
                       size_t *max_info_size)
{
    errno = ENOSYS;

    return BHT_ERROR;
}

int
os_socket_addr_local(bh_socket_t socket, bh_sockaddr_t *sockaddr)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    struct sockaddr_in addr = { 0 };
    int len = sizeof(addr);
    if (getsockname(socket->raw.socket, (struct sockaddr *)&addr, &len)
        == SOCKET_ERROR)
        return socket_error();
    return socket_address(&addr, sockaddr);
}

int
os_socket_set_send_timeout(bh_socket_t socket, uint64 timeout_us)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    errno = ENOSYS;

    return BHT_ERROR;
}

int
os_socket_get_send_timeout(bh_socket_t socket, uint64 *timeout_us)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    errno = ENOSYS;

    return BHT_ERROR;
}

int
os_socket_set_recv_timeout(bh_socket_t socket, uint64 timeout_us)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    uint64 ms = timeout_us / 1000 + (timeout_us % 1000 != 0);
    if (ms > INT32_MAX) {
        errno = EINVAL;
        return BHT_ERROR;
    }
    return socket_option(socket, SOL_SOCKET, SO_RCVTIMEO, (int)ms);
}

int
os_socket_get_recv_timeout(bh_socket_t socket, uint64 *timeout_us)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    errno = ENOSYS;

    return BHT_ERROR;
}

int
os_socket_addr_remote(bh_socket_t socket, bh_sockaddr_t *sockaddr)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    struct sockaddr_in addr = { 0 };
    int len = sizeof(addr);
    if (getpeername(socket->raw.socket, (struct sockaddr *)&addr, &len)
        == SOCKET_ERROR)
        return socket_error();
    return socket_address(&addr, sockaddr);
}

int
os_socket_set_send_buf_size(bh_socket_t socket, size_t bufsiz)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    errno = ENOSYS;

    return BHT_ERROR;
}

int
os_socket_get_send_buf_size(bh_socket_t socket, size_t *bufsiz)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    errno = ENOSYS;

    return BHT_ERROR;
}

int
os_socket_set_recv_buf_size(bh_socket_t socket, size_t bufsiz)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    errno = ENOSYS;

    return BHT_ERROR;
}

int
os_socket_get_recv_buf_size(bh_socket_t socket, size_t *bufsiz)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    errno = ENOSYS;

    return BHT_ERROR;
}

int
os_socket_set_keep_alive(bh_socket_t socket, bool is_enabled)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    errno = ENOSYS;

    return BHT_ERROR;
}

int
os_socket_get_keep_alive(bh_socket_t socket, bool *is_enabled)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    errno = ENOSYS;

    return BHT_ERROR;
}

int
os_socket_set_reuse_addr(bh_socket_t socket, bool is_enabled)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    return socket_option(socket, SOL_SOCKET, SO_REUSEADDR, is_enabled);
}

int
os_socket_get_reuse_addr(bh_socket_t socket, bool *is_enabled)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    errno = ENOSYS;

    return BHT_ERROR;
}

int
os_socket_set_reuse_port(bh_socket_t socket, bool is_enabled)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    errno = ENOSYS;

    return BHT_ERROR;
}

int
os_socket_get_reuse_port(bh_socket_t socket, bool *is_enabled)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    errno = ENOSYS;

    return BHT_ERROR;
}

int
os_socket_set_linger(bh_socket_t socket, bool is_enabled, int linger_s)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    errno = ENOSYS;

    return BHT_ERROR;
}

int
os_socket_get_linger(bh_socket_t socket, bool *is_enabled, int *linger_s)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    errno = ENOSYS;

    return BHT_ERROR;
}

int
os_socket_set_tcp_no_delay(bh_socket_t socket, bool is_enabled)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    return socket_option(socket, IPPROTO_TCP, TCP_NODELAY, is_enabled);
}

int
os_socket_get_tcp_no_delay(bh_socket_t socket, bool *is_enabled)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    errno = ENOSYS;

    return BHT_ERROR;
}

int
os_socket_set_tcp_quick_ack(bh_socket_t socket, bool is_enabled)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    errno = ENOSYS;

    return BHT_ERROR;
}

int
os_socket_get_tcp_quick_ack(bh_socket_t socket, bool *is_enabled)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    errno = ENOSYS;

    return BHT_ERROR;
}

int
os_socket_set_tcp_keep_idle(bh_socket_t socket, uint32 time_s)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    errno = ENOSYS;

    return BHT_ERROR;
}

int
os_socket_get_tcp_keep_idle(bh_socket_t socket, uint32 *time_s)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    errno = ENOSYS;

    return BHT_ERROR;
}

int
os_socket_set_tcp_keep_intvl(bh_socket_t socket, uint32 time_s)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    errno = ENOSYS;

    return BHT_ERROR;
}

int
os_socket_get_tcp_keep_intvl(bh_socket_t socket, uint32 *time_s)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    errno = ENOSYS;

    return BHT_ERROR;
}

int
os_socket_set_tcp_fastopen_connect(bh_socket_t socket, bool is_enabled)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    errno = ENOSYS;

    return BHT_ERROR;
}

int
os_socket_get_tcp_fastopen_connect(bh_socket_t socket, bool *is_enabled)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    errno = ENOSYS;

    return BHT_ERROR;
}

int
os_socket_set_ip_multicast_loop(bh_socket_t socket, bool ipv6, bool is_enabled)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    errno = ENOSYS;

    return BHT_ERROR;
}

int
os_socket_get_ip_multicast_loop(bh_socket_t socket, bool ipv6, bool *is_enabled)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    errno = ENOSYS;

    return BHT_ERROR;
}

int
os_socket_set_ip_add_membership(bh_socket_t socket,
                                bh_ip_addr_buffer_t *imr_multiaddr,
                                uint32_t imr_interface, bool is_ipv6)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    errno = ENOSYS;

    return BHT_ERROR;
}

int
os_socket_set_ip_drop_membership(bh_socket_t socket,
                                 bh_ip_addr_buffer_t *imr_multiaddr,
                                 uint32_t imr_interface, bool is_ipv6)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    errno = ENOSYS;

    return BHT_ERROR;
}

int
os_socket_set_ip_ttl(bh_socket_t socket, uint8_t ttl_s)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    errno = ENOSYS;

    return BHT_ERROR;
}

int
os_socket_get_ip_ttl(bh_socket_t socket, uint8_t *ttl_s)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    errno = ENOSYS;

    return BHT_ERROR;
}

int
os_socket_set_ip_multicast_ttl(bh_socket_t socket, uint8_t ttl_s)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    errno = ENOSYS;

    return BHT_ERROR;
}

int
os_socket_get_ip_multicast_ttl(bh_socket_t socket, uint8_t *ttl_s)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    errno = ENOSYS;

    return BHT_ERROR;
}

int
os_socket_set_ipv6_only(bh_socket_t socket, bool option)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    errno = ENOSYS;

    return BHT_ERROR;
}

int
os_socket_get_ipv6_only(bh_socket_t socket, bool *option)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    errno = ENOSYS;

    return BHT_ERROR;
}

int
os_socket_set_broadcast(bh_socket_t socket, bool is_enabled)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    return socket_option(socket, SOL_SOCKET, SO_BROADCAST, is_enabled);
}

int
os_socket_get_broadcast(bh_socket_t socket, bool *is_enabled)
{
    CHECK_VALID_SOCKET_HANDLE(socket);

    errno = ENOSYS;
    return BHT_ERROR;
}

/*
 * Copyright (c) 2017 Dariusz Stojaczyk. All Rights Reserved.
 * The following source code is released under an MIT-style license,
 * that can be found in the LICENSE file.
 */

#include <stdlib.h>
#include <stdatomic.h>
#include <stdio.h>
#include <zconf.h>
#include <assert.h>
#include <stdbool.h>
#include <memory.h>
#include <errno.h>
#include "network.h"
#include "log.h"

#define MAX_NETWORK_CONNECTIONS 32
#define CONNECTION_TIMEOUT_SEC 3

enum network_conn_state {
    NETWORK_CONN_STATE_UNITIALIZED = 0,
    NETWORK_CONN_STATE_DISCONNECTED,
    NETWORK_CONN_STATE_CONNECTED,
};

struct network_conn {
    int fd;
    bool server;
    enum network_conn_state state;
    struct sockaddr_in sin_me;
    struct sockaddr_in sin_oth;
    socklen_t slen;
};

static atomic_int g_conn_count;
static struct network_conn g_conns[MAX_NETWORK_CONNECTIONS];

static struct network_conn *
get_network_conn(int conn_id)
{
    struct network_conn *conn = NULL;

    if ((unsigned) conn_id < MAX_NETWORK_CONNECTIONS) {
        conn = &g_conns[conn_id];
    }

    return conn;
}

int
network_udp_init_conn(in_port_t port, bool server)
{
    int conn_id;
    struct network_conn *conn;
    struct timeval timeout;

    conn_id = atomic_fetch_add(&g_conn_count, 1);
    conn = &g_conns[conn_id];
    assert(conn->state == NETWORK_CONN_STATE_UNITIALIZED);
    
    conn->fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (conn->fd == -1) {
        perror("socket");
        return -1;
    }

    conn->server = server;
    
    timeout.tv_sec = CONNECTION_TIMEOUT_SEC;
    timeout.tv_usec = 0;

    if (setsockopt(conn->fd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout)) != 0) {
        perror("setsockopt recv");
    }

    if (setsockopt(conn->fd, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout, sizeof(timeout)) != 0) {
        perror("setsockopt send");
    }

    conn->sin_me.sin_family = AF_INET;
    conn->sin_me.sin_addr.s_addr = htonl(INADDR_ANY);
    conn->sin_me.sin_port = port;

    if (port > 0 && bind(conn->fd, (struct sockaddr *) &conn->sin_me, sizeof(conn->sin_me)) == -1) {
        perror("bind");
        close(conn->fd);
        return -1;
    }
    
    conn->state = NETWORK_CONN_STATE_DISCONNECTED;
    return conn_id;
}

int
network_udp_connect(int conn_id, in_addr_t addr, in_port_t port)
{
    struct network_conn *conn;

    conn = get_network_conn(conn_id);
    assert(conn->state == NETWORK_CONN_STATE_DISCONNECTED);

    conn->sin_oth.sin_addr.s_addr = addr;
    conn->sin_oth.sin_family = AF_INET;
    conn->sin_oth.sin_port = port;
    
    conn->slen = sizeof(conn->sin_oth);

    conn->state = NETWORK_CONN_STATE_CONNECTED;
    return 0;
}

int
network_udp_send(int conn_id, const void *buf, size_t len)
{
    struct network_conn *conn;
    ssize_t sent_bytes;
    char hexdump_line[64];

    conn = get_network_conn(conn_id);
    assert(conn->state == NETWORK_CONN_STATE_CONNECTED);
    
    sent_bytes = sendto(conn->fd, buf, len, 0, (struct sockaddr *) &conn->sin_oth, conn->slen);
    if (sent_bytes < 0) {
        perror("sendto");
    }

    snprintf(hexdump_line, sizeof(hexdump_line), "sent %zd/%zu bytes to %d", sent_bytes, len, ntohs(conn->sin_oth.sin_port));
    hexdump(hexdump_line, buf, len);
    
    return (int) sent_bytes;
}

int
network_udp_receive(int conn_id, void *buf, size_t len)
{
    struct network_conn *conn;
    ssize_t recv_bytes;
    struct sockaddr_in sin_oth_tmp;
    socklen_t slen;
    char hexdump_line[64];
    int rc;
    
    conn = get_network_conn(conn_id);
    assert((conn->server && conn->state != NETWORK_CONN_STATE_UNITIALIZED) ||
           (!conn->server && conn->state == NETWORK_CONN_STATE_CONNECTED));

    slen = sizeof(sin_oth_tmp);
    recv_bytes = recvfrom(conn->fd, buf, len, 0, (struct sockaddr *) &sin_oth_tmp, &slen);
    if (recv_bytes < 0) {
        rc = errno;
        if (rc != EAGAIN && rc != EWOULDBLOCK) {
            perror("recvfrom");
        }
        return -1;
    }

    snprintf(hexdump_line, sizeof(hexdump_line), "received %zd bytes from %d", recv_bytes, ntohs(sin_oth_tmp.sin_port));
    hexdump(hexdump_line, buf, recv_bytes);
    
    if (conn->server && conn->state == NETWORK_CONN_STATE_DISCONNECTED) {
        network_udp_connect(conn_id, sin_oth_tmp.sin_addr.s_addr, sin_oth_tmp.sin_port);
    }
    
    return (int) recv_bytes;
}

int
network_udp_disconnect(int conn_id)
{
    struct network_conn *conn;

    conn = get_network_conn(conn_id);
    assert(conn->state == NETWORK_CONN_STATE_CONNECTED);
    
    /* dummy function */
    
    conn->state = NETWORK_CONN_STATE_DISCONNECTED;
    return 0;
}

int
network_udp_free(int conn_id)
{
    struct network_conn *conn;

    conn = get_network_conn(conn_id);
    assert(conn->state == NETWORK_CONN_STATE_DISCONNECTED);

    close(conn->fd);
    conn->state = NETWORK_CONN_STATE_UNITIALIZED;
    return 0;
}

int
network_tcp_init_conn(in_port_t port, bool server)
{
    int conn_id;
    struct network_conn *conn;
    struct timeval timeout;
    int one = 1;

    conn_id = atomic_fetch_add(&g_conn_count, 1);
    conn = &g_conns[conn_id];
    assert(conn->state == NETWORK_CONN_STATE_UNITIALIZED);

    conn->fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (conn->fd == -1) {
        perror("socket");
        return -1;
    }

    conn->server = false; /* server not supported yet */

    timeout.tv_sec = CONNECTION_TIMEOUT_SEC;
    timeout.tv_usec = 0;

    if (setsockopt(conn->fd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout)) != 0) {
        perror("setsockopt recv");
    }

    if (setsockopt(conn->fd, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout, sizeof(timeout)) != 0) {
        perror("setsockopt send");
    }

    if (setsockopt(conn->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(int)) != 0) {
        perror("setsockopt");
    }
    
    conn->sin_me.sin_family = AF_INET;
    conn->sin_me.sin_addr.s_addr = htonl(INADDR_ANY);
    conn->sin_me.sin_port = port;

    if (port > 0 && bind(conn->fd, (struct sockaddr *) &conn->sin_me, sizeof(conn->sin_me)) != 0) {
        perror("bind");
        close(conn->fd);
        return -1;
    }

    conn->state = NETWORK_CONN_STATE_DISCONNECTED;
    return conn_id;
}

int
network_tcp_connect(int conn_id, in_addr_t addr, in_port_t port)
{
    struct network_conn *conn;

    conn = get_network_conn(conn_id);
    assert(conn->state == NETWORK_CONN_STATE_DISCONNECTED);

    conn->sin_oth.sin_addr.s_addr = addr;
    conn->sin_oth.sin_family = AF_INET;
    conn->sin_oth.sin_port = port;

    conn->slen = sizeof(conn->sin_oth);

    if (connect(conn->fd , (struct sockaddr *)&conn->sin_oth , conn->slen) != 0) {
        perror("connect");
        return -1;
    }
    
    conn->state = NETWORK_CONN_STATE_CONNECTED;
    return 0;
}

int
network_tcp_send(int conn_id, const void *buf, size_t len)
{
    struct network_conn *conn;
    ssize_t sent_bytes;
    char hexdump_line[64];
    
    conn = get_network_conn(conn_id);
    assert(conn->state == NETWORK_CONN_STATE_CONNECTED);

    sent_bytes = send(conn->fd, buf, len, 0);
    if (sent_bytes < 0) {
        perror("sendto");
    }
    
    snprintf(hexdump_line, sizeof(hexdump_line), "sent %zd/%zu bytes to %d", sent_bytes, len, ntohs(conn->sin_oth.sin_port));
    hexdump(hexdump_line, buf, len);

    return (int) sent_bytes;
}

int
network_tcp_receive(int conn_id, void *buf, size_t len)
{
    struct network_conn *conn;
    ssize_t recv_bytes;
    char hexdump_line[64];
    int rc;

    conn = get_network_conn(conn_id);
    assert(conn->state == NETWORK_CONN_STATE_CONNECTED);

    recv_bytes = recv(conn->fd, buf, len, 0);
    rc = errno;
    if (recv_bytes < 0) {
        if (rc != EAGAIN && rc != EWOULDBLOCK) {
            perror("recvfrom");
        }
        return -1;
    }

    snprintf(hexdump_line, sizeof(hexdump_line), "received %zd bytes from %d", recv_bytes, ntohs(conn->sin_oth.sin_port));
    hexdump(hexdump_line, buf, recv_bytes);

    return (int) recv_bytes;
}

int
network_tcp_disconnect(int conn_id)
{
    struct network_conn *conn;

    conn = get_network_conn(conn_id);
    assert(conn->state == NETWORK_CONN_STATE_CONNECTED);

    close(conn->fd);
    conn->state = NETWORK_CONN_STATE_DISCONNECTED;
    return 0;
}

int
network_tcp_free(int conn_id)
{
    struct network_conn *conn;

    conn = get_network_conn(conn_id);
    assert(conn->state == NETWORK_CONN_STATE_DISCONNECTED);

    conn->state = NETWORK_CONN_STATE_UNITIALIZED;
    return 0;
}

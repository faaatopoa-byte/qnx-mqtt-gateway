/*
 * qnx_ipc_host.c — host stub backend for qnx_ipc.h (see DESIGN.md).
 *
 * Emulates QNX message-passing semantics over Unix-domain stream sockets,
 * ONLY for host-side logic tests. NEVER shipped to the target.
 *
 * Mapping:
 *  - create(name) -> bind+listen on /tmp/qnx-ipc-<name>.sock (name is
 *    sanitised to [A-Za-z0-9_.-]; any other character is rejected).
 *    name == NULL: an anonymous channel (socketpair); lookup() of it
 *    is impossible, wake() still works.
 *  - lookup(name) -> connect() to the socket; the fd IS the "coid".
 *  - send(coid, ...) -> write a 4-byte big-endian length + payload, then
 *    read a 4-byte length + reply. The peer stays "blocked" in send()
 *    until the server replies — same as MsgSend.
 *  - receive(ch, ...) -> poll() the listen fd, all open connections and
 *    the wake pipe up to timeout_ms; read one length-prefixed message
 *    from whichever connection has one. The connection fd is the
 *    "rcvid": it stays open after reply() so the same client coid can
 *    send again (like a QNX connection id). A wake connection sends the
 *    4-byte marker 0xFFFFFFFF instead of a length, and receive()
 *    returns QNX_IPC_PULSE.
 *  - reply(rcvid, ...) -> write length-prefixed reply to the connection
 *    fd; the fd stays open for further round-trips.
 *  - wake(ch) -> connect to our own socket and send the wake marker.
 *
 * Framing on the stream socket is length-prefixed because, unlike a QNX
 * message, a stream has no boundaries. The 4-byte length prefix is an
 * artifact of the stub only — the framed qnx_mqtt_msg_t payload inside
 * is byte-identical to the QNX path.
 *
 * Threading: one thread receives on a channel (the plugin worker), as
 * on QNX. send() is safe from any thread; each lookup() yields its own
 * connection/fd.
 */
#ifndef __QNX__

#define _POSIX_C_SOURCE 200809L /* clock_gettime() */
#include "qnx_ipc.h"

#include <time.h>

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>

#define QNX_IPC_WAKE_MARKER 0xFFFFFFFFu
#define QNX_IPC_MAX_MSG     65536 /* stub transport cap, above the 4096 frame cap */

struct qnx_ipc_channel {
    int listen_fd;      /* listening socket, or -1 for anonymous */
    int conns[16];      /* accepted connections still open (see below) */
    int nconns;
    char path[108];     /* socket path (sun_path limit), or "" */
    int wake_r;         /* read end of wake self-pipe, or -1 */
    int wake_w;         /* write end of wake self-pipe, or -1 */
};

static int ipc_debug(void)
{
    static int cached = -1;
    if (cached < 0) cached = getenv("QNX_IPC_DEBUG") ? 1 : 0;
    return cached;
}

static void ipc_log(const char *what)
{
    if (ipc_debug()) fprintf(stderr, "qnx_ipc(host): %s: %s\n", what, strerror(errno));
}

/* Validate a channel name and build its socket path. */
static int socket_path(const char *name, char *out, size_t cap)
{
    size_t i, n;
    if (!name || !*name) return -1;
    n = strlen(name);
    if (n > 64) return -1;
    for (i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
              || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
            return -1;
    }
    if (snprintf(out, cap, "/tmp/qnx-ipc-%s.sock", name) >= (int)cap) return -1;
    return 0;
}

static int write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int read_all(int fd, void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;
    while (len > 0) {
        ssize_t n = recv(fd, p, len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1; /* peer closed early */
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

int qnx_ipc_create(const char *name, qnx_ipc_channel_t **out)
{
    qnx_ipc_channel_t *ch;
    struct sockaddr_un addr;
    int fds[2];

    if (!out) return QNX_IPC_ERR;
    *out = NULL;

    ch = calloc(1, sizeof(*ch));
    if (!ch) return QNX_IPC_ERR;
    ch->listen_fd = -1;
    ch->nconns = 0;
    for (int i = 0; i < 16; i++) ch->conns[i] = -1;
    ch->wake_r = -1;
    ch->wake_w = -1;

    /* Wake self-pipe: lets wake() unblock a receive() that is sitting in
     * poll() on the listen fd (anonymous channels have no socket path). */
    if (pipe(fds) == 0) {
        ch->wake_r = fds[0];
        ch->wake_w = fds[1];
    }

    if (name) {
        if (socket_path(name, ch->path, sizeof(ch->path)) != 0) {
            ipc_log("create: bad channel name");
            goto fail;
        }
        unlink(ch->path); /* stale socket from a previous run */
        ch->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (ch->listen_fd < 0) { ipc_log("socket"); goto fail; }
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", ch->path);
        if (bind(ch->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            ipc_log("bind");
            goto fail;
        }
        if (listen(ch->listen_fd, 16) != 0) {
            ipc_log("listen");
            goto fail;
        }
    }

    *out = ch;
    return QNX_IPC_OK;

fail:
    qnx_ipc_destroy(ch);
    return QNX_IPC_ERR;
}

void qnx_ipc_destroy(qnx_ipc_channel_t *ch)
{
    if (!ch) return;
    for (int i = 0; i < ch->nconns; i++)
        if (ch->conns[i] >= 0) close(ch->conns[i]);
    if (ch->listen_fd >= 0) close(ch->listen_fd);
    if (ch->wake_r >= 0) close(ch->wake_r);
    if (ch->wake_w >= 0) close(ch->wake_w);
    if (ch->path[0]) unlink(ch->path);
    free(ch);
}

int qnx_ipc_lookup(const char *name, int *coid_out)
{
    char path[108];
    struct sockaddr_un addr;
    int fd;

    if (!name || !coid_out) return QNX_IPC_ERR;
    if (socket_path(name, path, sizeof(path)) != 0) return QNX_IPC_ERR;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { ipc_log("lookup socket"); return QNX_IPC_ERR; }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ipc_log("lookup connect");
        close(fd);
        return QNX_IPC_ERR;
    }
    *coid_out = fd;
    return QNX_IPC_OK;
}

int qnx_ipc_send(int coid, const void *buf, size_t len,
                 void *reply, size_t reply_cap, size_t *reply_len)
{
    uint8_t hdr[4];
    uint32_t rlen;

    if (coid < 0 || !buf || len == 0 || len > QNX_IPC_MAX_MSG)
        return QNX_IPC_ERR;
    if (reply_cap > 0 && !reply) return QNX_IPC_ERR;

    put_be32(hdr, (uint32_t)len);
    if (write_all(coid, hdr, 4) != 0 || write_all(coid, buf, len) != 0) {
        ipc_log("send write");
        return QNX_IPC_ERR;
    }
    /* Block until the server replies (MsgSend semantics). */
    if (read_all(coid, hdr, 4) != 0) {
        ipc_log("send read reply header");
        return QNX_IPC_ERR;
    }
    rlen = get_be32(hdr);
    if (rlen > reply_cap) {
        fprintf(stderr, "qnx_ipc(host): reply %u bytes exceeds cap %zu\n",
                rlen, reply_cap);
        return QNX_IPC_ERR;
    }
    if (rlen > 0 && read_all(coid, reply, rlen) != 0) {
        ipc_log("send read reply");
        return QNX_IPC_ERR;
    }
    if (reply_len) *reply_len = rlen;
    return QNX_IPC_OK;
}

int qnx_ipc_receive(qnx_ipc_channel_t *ch, void *buf, size_t cap,
                    size_t *len_out, int *rcvid_out, int timeout_ms)
{
    /* Persistent connections: a client "coid" (one connected fd) stays
     * usable for many send()/reply() round-trips, exactly like a QNX
     * connection id. So an accepted fd is kept open after reply() and
     * this function polls ALL open connections plus the listen fd and
     * the wake pipe together. A connection whose peer closed is
     * dropped silently; the timeout budget covers the whole call
     * (deadline-based, so EOF churn cannot stretch it). */
    struct timespec deadline;
    int have_deadline = 0;

    if (!ch || !buf || cap == 0 || !len_out || !rcvid_out)
        return QNX_IPC_ERR;
    if (timeout_ms >= 0) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        deadline.tv_sec = now.tv_sec + timeout_ms / 1000;
        deadline.tv_nsec = now.tv_nsec + (long)(timeout_ms % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec += 1;
            deadline.tv_nsec -= 1000000000L;
        }
        have_deadline = 1;
    }

    for (;;) {
        struct pollfd pfds[2 + 16];
        int nfds = 0;
        int listen_idx = -1, wake_idx = -1;
        int ms = -1;
        int rc, i;

        if (have_deadline) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long rem = (long)(deadline.tv_sec - now.tv_sec) * 1000
                     + (long)(deadline.tv_nsec - now.tv_nsec) / 1000000L;
            if (rem < 0) rem = 0;
            ms = (int)rem;
        }

        if (ch->listen_fd >= 0) {
            listen_idx = nfds;
            pfds[nfds].fd = ch->listen_fd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }
        if (ch->wake_r >= 0) {
            wake_idx = nfds;
            pfds[nfds].fd = ch->wake_r;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }
        int conn_base = nfds;
        for (i = 0; i < ch->nconns; i++) {
            pfds[nfds].fd = ch->conns[i];
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }
        if (nfds == 0) return QNX_IPC_ERR; /* nothing to wait on */

        rc = poll(pfds, (nfds_t)nfds, ms);
        if (rc < 0) {
            if (errno == EINTR) continue;
            ipc_log("receive poll");
            return QNX_IPC_ERR;
        }
        if (rc == 0) return QNX_IPC_TIMEOUT;

        /* Wake pipe readable? Drain one read and report a pulse. */
        if (wake_idx >= 0 && (pfds[wake_idx].revents & POLLIN)) {
            uint8_t drain[64];
            ssize_t nread = read(ch->wake_r, drain, sizeof(drain));
            (void)nread; /* value irrelevant; readability is the signal */
            return QNX_IPC_PULSE;
        }

        /* New connection waiting? Accept it and loop to poll it. */
        if (listen_idx >= 0 && (pfds[listen_idx].revents & POLLIN)) {
            int fd = accept(ch->listen_fd, NULL, NULL);
            if (fd >= 0) {
                if (ch->nconns < 16) {
                    ch->conns[ch->nconns++] = fd;
                } else {
                    fprintf(stderr, "qnx_ipc(host): too many connections\n");
                    close(fd);
                }
            } else if (errno != EINTR && errno != EAGAIN) {
                ipc_log("receive accept");
                return QNX_IPC_ERR;
            }
            continue;
        }

        /* A message (or a dead peer) on an open connection. */
        for (i = conn_base; i < nfds; i++) {
            int fd;
            uint8_t hdr[4];
            uint32_t len;
            int ci = i - conn_base;
            if (ci >= ch->nconns) break;
            fd = ch->conns[ci];
            if (!(pfds[i].revents & (POLLIN | POLLERR | POLLHUP))) continue;
            if (read_all(fd, hdr, 4) != 0) {
                /* Peer closed or errored: drop the connection. */
                close(fd);
                ch->conns[ci] = ch->conns[--ch->nconns];
                break; /* re-poll with the remaining budget */
            }
            len = get_be32(hdr);
            if (len == QNX_IPC_WAKE_MARKER) {
                close(fd);
                ch->conns[ci] = ch->conns[--ch->nconns];
                return QNX_IPC_PULSE;
            }
            if (len == 0 || len > cap || len > QNX_IPC_MAX_MSG) {
                fprintf(stderr,
                        "qnx_ipc(host): message %u bytes exceeds cap %zu\n",
                        len, cap);
                close(fd);
                ch->conns[ci] = ch->conns[--ch->nconns];
                break;
            }
            if (read_all(fd, buf, len) != 0) {
                close(fd);
                ch->conns[ci] = ch->conns[--ch->nconns];
                break;
            }
            /* Keep the fd open: the same coid will send again, and
             * reply() answers on it without closing. */
            *len_out = len;
            *rcvid_out = fd; /* fd doubles as the rcvid on the host */
            return QNX_IPC_OK;
        }
        /* Nothing consumed (only a dropped connection): re-poll. */
    }
}

int qnx_ipc_reply(int rcvid, const void *buf, size_t len)
{
    uint8_t hdr[4];

    if (rcvid < 0) return QNX_IPC_ERR;
    if (len > 0 && !buf) return QNX_IPC_ERR;
    if (len > QNX_IPC_MAX_MSG) return QNX_IPC_ERR;

    put_be32(hdr, (uint32_t)len);
    if (write_all(rcvid, hdr, 4) != 0
        || (len > 0 && write_all(rcvid, buf, len) != 0)) {
        ipc_log("reply write");
        return QNX_IPC_ERR;
    }
    /* Do NOT close: the fd is the client's coid and stays usable for
     * further send()/reply() round-trips (QNX semantics). It is closed
     * when the peer disconnects or the channel is destroyed. */
    return QNX_IPC_OK;
}

int qnx_ipc_wake(qnx_ipc_channel_t *ch)
{
    uint8_t marker = 0;

    if (!ch) return QNX_IPC_ERR;
    /* Wake via the self-pipe (works for named and anonymous channels).
     * The byte value is irrelevant; readability is the signal. */
    if (ch->wake_w >= 0) {
        ssize_t n;
        do {
            n = write(ch->wake_w, &marker, 1);
        } while (n < 0 && errno == EINTR);
        if (n == 1) return QNX_IPC_OK;
        /* If the pipe is full the receiver is already awake; fall through
         * to the socket path for named channels. */
    }
    if (ch->path[0]) {
        /* Socket wake: connect and send the wake marker, then close. */
        struct sockaddr_un addr;
        uint8_t hdr[4];
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return QNX_IPC_ERR;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", ch->path);
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            close(fd);
            return QNX_IPC_ERR;
        }
        put_be32(hdr, QNX_IPC_WAKE_MARKER);
        (void)write_all(fd, hdr, 4);
        close(fd);
        return QNX_IPC_OK;
    }
    return QNX_IPC_ERR;
}

#endif /* !__QNX__ */

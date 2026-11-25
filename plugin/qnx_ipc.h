/*
 * qnx_ipc.h — IPC abstraction for the qnx-mqtt-gateway (see DESIGN.md).
 *
 * Identical API on QNX and on the host stub. Two backends:
 *   qnx_ipc_qnx.c  — real QNX: ChannelCreate/name_attach/name_open,
 *                    MsgSend/MsgReceive/MsgReply, TimerTimeout wakeups.
 *                    (NOT compiled on the host.)
 *   qnx_ipc_host.c — host stub: Unix-domain socket, same semantics.
 *                    ONLY for host-side logic tests; never shipped to target.
 *
 * Return conventions: 0 = ok; qnx_ipc_receive returns 1 on timeout,
 * 2 on pulse/wake (caller ignores it), <0 on error. All functions log to
 * stderr on error when QNX_IPC_DEBUG is set in the environment (host) or
 * when the plugin log level is on (target).
 *
 * Semantics each backend must preserve:
 *  - create() owns a named receive channel; destroy() releases it.
 *  - lookup() returns a "coid" usable with send() until the peer goes away.
 *  - send() delivers one message and blocks until the peer replies
 *    (or the peer dies / the send fails); *reply_len is the reply size.
 *  - receive() blocks up to timeout_ms (0 = poll once, <0 = block
 *    forever); on success *len_out is the message size and *rcvid_out
 *    identifies the sender for reply(). A message is only consumed by
 *    receive(); the sender stays blocked until reply() (or the channel
 *    is destroyed).
 *  - wake() unblocks a receive() blocked on the same channel with a
 *    pulse return (2). Used for clean worker-thread shutdown.
 */
#ifndef QNX_IPC_H
#define QNX_IPC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct qnx_ipc_channel qnx_ipc_channel_t; /* opaque */

#define QNX_IPC_OK       0
#define QNX_IPC_TIMEOUT  1  /* qnx_ipc_receive: no message within timeout_ms */
#define QNX_IPC_PULSE    2  /* qnx_ipc_receive: pulse/wake, no message */
#define QNX_IPC_ERR     -1  /* generic error (see errno / stderr log) */

int  qnx_ipc_create(const char *name, qnx_ipc_channel_t **out);
void qnx_ipc_destroy(qnx_ipc_channel_t *ch);
int  qnx_ipc_lookup(const char *name, int *coid_out);
int  qnx_ipc_send(int coid, const void *buf, size_t len,
                  void *reply, size_t reply_cap, size_t *reply_len);
int  qnx_ipc_receive(qnx_ipc_channel_t *ch, void *buf, size_t cap,
                     size_t *len_out, int *rcvid_out, int timeout_ms);
int  qnx_ipc_reply(int rcvid, const void *buf, size_t len);
int  qnx_ipc_wake(qnx_ipc_channel_t *ch); /* pulse own channel: unblock receive */

#ifdef __cplusplus
}
#endif

#endif /* QNX_IPC_H */

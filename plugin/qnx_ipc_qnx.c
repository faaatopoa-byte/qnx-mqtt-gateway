/*
 * qnx_ipc_qnx.c — QNX backend for qnx_ipc.h (see DESIGN.md).
 *
 * *** THIS FILE IS NOT COMPILED ON THE HOST. ***
 * It is only built by plugin/Makefile.qnx with qcc once the QNX SDP 8.0
 * toolchain lands (~/qnx/qnx800/ is still empty at the time of writing).
 * It has NOT been compiled or tested yet — read it carefully against the
 * QNX 8.0 docs (ChannelCreate, name_attach, MsgSend, MsgReceive,
 * MsgReply, TimerTimeout, MsgSendPulse) before the first qcc run.
 *
 * Mapping to the qnx_ipc.h API:
 *  - create  -> ChannelCreate(0) + name_attach(NULL, name, 0) for a named
 *               channel (name == NULL: anonymous ChannelCreate only).
 *               Also ConnectAttach()s to our own channel so TimerTimeout
 *               can pulse us and qnx_ipc_wake() can MsgSendPulse us.
 *  - lookup  -> name_open(name, 0); the returned coid is used by send().
 *  - send    -> MsgSend(coid, ...). Returns 0 on success; MsgSend returns
 *               the status the peer passed to MsgReply(), so a peer reply
 *               status != 0 is treated as an error here (the bridge's
 *               1-byte status is 0 = ok).
 *  - receive -> TimerTimeout(CLOCK_REALTIME, _NTO_TIMEOUT_RECEIVE, pulse
 *               event, 100 ms) then MsgReceive(). Timeout expiry and
 *               qnx_ipc_wake() both arrive as pulses (rcvid == 0):
 *               timeout pulse code -> QNX_IPC_TIMEOUT (1),
 *               wake pulse code    -> QNX_IPC_PULSE (2),
 *               any other pulse    -> ignored, receive again immediately.
 *               A real message (rcvid > 0) yields *len_out from
 *               _msg_info.msglen.
 *  - reply   -> MsgReply(rcvid, 0, buf, len).
 *  - wake    -> MsgSendPulse(own coid, _NTO_PULSE_PRIO_INHERIT,
 *               QNX_IPC_WAKE_CODE, 0).
 *
 * Threading: exactly one thread (the plugin worker) calls receive() on a
 * channel, so TimerTimeout — which is per-thread state — is safe here.
 * send() may be called from the worker thread only in v1.
 */
#ifdef __QNX__
/* The whole file is QNX-only; compiling it on the host is a hard error
 * rather than a silent empty translation unit. */
#include "qnx_ipc.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <sys/siginfo.h>
#include <sys/types.h>

/* Pulse codes delivered to our own channel. Must lie in the
 * _PULSE_CODE_MINAVAIL.._PULSE_CODE_MAXAVAIL range. */
#define QNX_IPC_TIMEOUT_CODE (_PULSE_CODE_MINAVAIL + 0)
#define QNX_IPC_WAKE_CODE    (_PULSE_CODE_MINAVAIL + 1)

#ifndef _NTO_PULSE_PRIO_INHERIT
#  define _NTO_PULSE_PRIO_INHERIT (-1)
#endif

struct qnx_ipc_channel {
    int chid;               /* ChannelCreate / name_attach chid */
    name_attach_t *attach;  /* NULL for an anonymous channel */
    int pulse_coid;         /* ConnectAttach to our own channel */
    char *name;             /* strdup'd channel name, or NULL */
};

static void ipc_log(const char *what, int err)
{
    fprintf(stderr, "qnx_ipc: %s: %s\n", what,
            err ? strerror(err) : "failed");
}

int qnx_ipc_create(const char *name, qnx_ipc_channel_t **out)
{
    qnx_ipc_channel_t *ch;

    if (!out) return QNX_IPC_ERR;
    *out = NULL;

    ch = calloc(1, sizeof(*ch));
    if (!ch) return QNX_IPC_ERR;
    ch->chid = -1;
    ch->pulse_coid = -1;

    if (name) {
        ch->attach = name_attach(NULL, name, 0);
        if (!ch->attach) {
            ipc_log("name_attach", errno);
            free(ch);
            return QNX_IPC_ERR;
        }
        ch->chid = ch->attach->chid;
        ch->name = strdup(name);
        if (!ch->name) {
            name_detach(ch->attach, 0);
            free(ch);
            return QNX_IPC_ERR;
        }
    } else {
        ch->chid = ChannelCreate(0);
        if (ch->chid == -1) {
            ipc_log("ChannelCreate", errno);
            free(ch);
            return QNX_IPC_ERR;
        }
    }

    /* Connection to our own channel: TimerTimeout pulses and
     * qnx_ipc_wake() pulses are delivered through this coid. */
    ch->pulse_coid = ConnectAttach(ND_LOCAL_NODE, 0, ch->chid,
                                   _NTO_SIDE_CHANNEL, 0);
    if (ch->pulse_coid == -1) {
        ipc_log("ConnectAttach(self)", errno);
        if (ch->attach) name_detach(ch->attach, 0);
        else ChannelDestroy(ch->chid);
        free(ch->name);
        free(ch);
        return QNX_IPC_ERR;
    }

    *out = ch;
    return QNX_IPC_OK;
}

void qnx_ipc_destroy(qnx_ipc_channel_t *ch)
{
    if (!ch) return;
    if (ch->pulse_coid != -1) ConnectDetach(ch->pulse_coid);
    if (ch->attach) name_detach(ch->attach, 0);
    else if (ch->chid != -1) ChannelDestroy(ch->chid);
    free(ch->name);
    free(ch);
}

int qnx_ipc_lookup(const char *name, int *coid_out)
{
    int coid;

    if (!name || !coid_out) return QNX_IPC_ERR;
    coid = name_open(name, 0);
    if (coid == -1) {
        ipc_log("name_open", errno);
        return QNX_IPC_ERR;
    }
    *coid_out = coid;
    return QNX_IPC_OK;
}

int qnx_ipc_send(int coid, const void *buf, size_t len,
                 void *reply, size_t reply_cap, size_t *reply_len)
{
    int rc;

    if (coid < 0 || !buf || len == 0) return QNX_IPC_ERR;
    if (reply_cap > 0 && !reply) return QNX_IPC_ERR;

    /* MsgSend returns the status the peer passed to MsgReply()
     * (0 = ok in our protocol); -1 with errno on failure. */
    rc = MsgSend(coid, buf, len, reply, reply_cap);
    if (rc == -1) {
        ipc_log("MsgSend", errno);
        return QNX_IPC_ERR;
    }
    if (rc != 0) {
        fprintf(stderr, "qnx_ipc: MsgSend: peer replied status %d\n", rc);
        return QNX_IPC_ERR;
    }
    if (reply_len) {
        /* MsgSend does not report the reply length. Best effort: our
         * replies are framed qnx_mqtt_msg_t, so if the reply buffer
         * holds a valid header, the frame's own lengths give the size.
         * Callers that only need the 1-byte status can ignore this. */
        *reply_len = 0;
        if (reply && reply_cap >= 12) {
            const uint8_t *r = (const uint8_t *)reply;
            uint32_t magic, tlen, plen;
            memcpy(&magic, r + 0, 4);
            memcpy(&tlen, r + 4, 4);
            memcpy(&plen, r + 8, 4);
            if (magic == 0x514D4252u /* 'QMBR' */
                && (size_t)12 + tlen + plen <= reply_cap)
                *reply_len = (size_t)12 + tlen + plen;
        }
    }
    return QNX_IPC_OK;
}

int qnx_ipc_receive(qnx_ipc_channel_t *ch, void *buf, size_t cap,
                    size_t *len_out, int *rcvid_out, int timeout_ms)
{
    struct _msg_info info;
    _Uint64t ntime_ns;  /* SDP 8.0 TimerTimeout takes nanoseconds, not timespec */
    int rcvid;

    if (!ch || !buf || cap == 0 || !len_out || !rcvid_out)
        return QNX_IPC_ERR;

    for (;;) {
        if (timeout_ms >= 0) {
            /* Docs' recommended form: notify=NULL -> SIGEV_UNBLOCK.
             * The worker only needs MsgReceive to return after the
             * budget so it can drain the outbound queue; it does not
             * need a timeout pulse. Wake pulses (qnx_ipc_wake) still
             * arrive on the channel as rcvid==0 pulses.
             * (The pulse-event form was dropped: it failed EINVAL in
             * the v5 QEMU run; see TIMER-TIMEOUT-DEBUG.md §3.) */
            /* SDP 8.0: TimerTimeout's 4th arg is const _Uint64t*
             * (nanoseconds), NOT struct timespec* — passing a
             * timespec is a type error (and wrong at runtime). */
            ntime_ns = (uint64_t)timeout_ms * 1000000ULL;
            if (TimerTimeout(CLOCK_REALTIME, _NTO_TIMEOUT_RECEIVE,
                             NULL, &ntime_ns, NULL) == -1) {
                ipc_log("TimerTimeout", errno);
                return QNX_IPC_ERR;
            }
        }

        rcvid = MsgReceive(ch->chid, buf, (int)cap, &info);
        if (rcvid == -1) {
            /* SIGEV_UNBLOCK: the armed timeout fails MsgReceive
             * with ETIMEDOUT instead of delivering a pulse. */
            if (errno == ETIMEDOUT)
                return QNX_IPC_TIMEOUT;
            ipc_log("MsgReceive", errno);
            return QNX_IPC_ERR;
        }
        if (rcvid == 0) {
            /* Pulse. The first bytes of buf are a struct _pulse. */
            struct _pulse *pulse = (struct _pulse *)buf;
            if (pulse->code == QNX_IPC_TIMEOUT_CODE)
                return QNX_IPC_TIMEOUT;
            if (pulse->code == QNX_IPC_WAKE_CODE)
                return QNX_IPC_PULSE;
            /* Any other pulse (e.g. _PULSE_CODE_DISCONNECT for a
             * dead client) is ignored; loop and receive again.
             * Note: the timeout is re-armed at the top of the next
             * iteration, so a foreign pulse does NOT reset the
             * 100 ms budget. (The QNX_IPC_TIMEOUT_CODE branch above
             * is defensive only — with SIGEV_UNBLOCK no timeout
             * pulse is ever armed; timeouts arrive as ETIMEDOUT.) */
            continue;
        }
        /* Real message. */
        *len_out = (size_t)info.msglen;
        *rcvid_out = rcvid;
        return QNX_IPC_OK;
    }
}

int qnx_ipc_reply(int rcvid, const void *buf, size_t len)
{
    if (rcvid <= 0) return QNX_IPC_ERR;
    if (len > 0 && !buf) return QNX_IPC_ERR;
    /* Status 0 = ok; the reply payload carries the bytes (if any). */
    if (MsgReply(rcvid, 0, buf, (int)len) == -1) {
        ipc_log("MsgReply", errno);
        return QNX_IPC_ERR;
    }
    return QNX_IPC_OK;
}

int qnx_ipc_wake(qnx_ipc_channel_t *ch)
{
    if (!ch || ch->pulse_coid == -1) return QNX_IPC_ERR;
    if (MsgSendPulse(ch->pulse_coid, _NTO_PULSE_PRIO_INHERIT,
                     QNX_IPC_WAKE_CODE, 0) == -1) {
        ipc_log("MsgSendPulse(wake)", errno);
        return QNX_IPC_ERR;
    }
    return QNX_IPC_OK;
}

#else /* !__QNX__ */
#  error "qnx_ipc_qnx.c is the QNX backend: build it only with qcc (see plugin/Makefile.qnx). On the host, build qnx_ipc_host.c instead."
#endif /* __QNX__ */

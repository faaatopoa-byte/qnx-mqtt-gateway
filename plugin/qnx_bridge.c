/*
 * qnx_bridge.c — Mosquitto v5 plugin bridging MQTT topics to QNX native
 * message passing (see DESIGN.md and qnx_bridge.h).
 *
 * Verified against third-party/mosquitto/upstream-mosquitto_plugin.h (420 lines) and
 * upstream-mosquitto_broker.h (561 lines), byte-identical to the
 * qnx-ports/mosquitto v2.0.15 fork. Every broker function used here was
 * checked against those headers, not from memory:
 *   mosquitto_plugin_version / init / cleanup (plugin.h),
 *   mosquitto_callback_register / unregister, mosquitto_broker_publish,
 *   mosquitto_log_printf (broker.h),
 *   MOSQ_EVT_MESSAGE / MOSQ_EVT_TICK / MOSQ_EVT_DISCONNECT,
 *   struct mosquitto_evt_message, MOSQ_ERR_SUCCESS, MOSQ_LOG_* (mosquitto.h).
 *
 * RULE (DESIGN.md): the broker is a single-threaded poll() loop, so ALL
 * blocking QNX IPC runs in the worker thread. Hooks never call MsgSend:
 * MOSQ_EVT_MESSAGE only enqueues; MOSQ_EVT_TICK only publishes.
 */
#define _POSIX_C_SOURCE 200809L /* usleep() */
#define _DEFAULT_SOURCE /* usleep() on glibc with -std=c11 */
#include <mosquitto_plugin.h>
#include <mosquitto.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "qnx_bridge.h"
#include "qnx_ipc.h"
#include "../common/qnx_msg.h"

/* ------------------------------------------------------------------ */
/* Logging                                                             */
/* ------------------------------------------------------------------ */

static bridge_state_t *g_state; /* fallback when a callback gets NULL userdata */

static void bridge_log(bridge_state_t *st, int level, const char *fmt, ...)
{
    va_list ap;
    if (!st || !st->log_enabled) return;
    va_start(ap, fmt);
    /* mosquitto_log_printf takes a printf-style format + varargs; there
     * is no vprintf variant in the plugin API, so format locally. */
    {
        char buf[512];
        vsnprintf(buf, sizeof(buf), fmt, ap);
        mosquitto_log_printf(level, "qnx_bridge: %s", buf);
    }
    va_end(ap);
}

/* ------------------------------------------------------------------ */
/* Topic filter matching                                               */
/* ------------------------------------------------------------------ */

/* Advance *p past one '/'-separated level; return the level start/len.
 * Returns 0 when there is no level left. */
static int next_level(const char **p, const char **start, size_t *len)
{
    const char *s;
    if (!p || !*p || !**p) return 0;
    s = *p;
    *start = s;
    while (**p && **p != '/') (*p)++;
    *len = (size_t)(*p - s);
    if (**p == '/') (*p)++; /* skip separator */
    return 1;
}

int bridge_topic_matches(const char *filter, const char *topic)
{
    const char *f, *t;

    if (!filter || !topic) return 0;
    if (strcmp(filter, "#") == 0) return 1;

    f = filter;
    t = topic;
    for (;;) {
        const char *fl, *tl;
        size_t flen, tlen;
        int have_f = next_level(&f, &fl, &flen);
        int have_t = next_level(&t, &tl, &tlen);

        if (!have_f) return !have_t; /* both exhausted -> match */
        if (flen == 1 && fl[0] == '#') return 1; /* '#' swallows the rest */
        if (!have_t) return 0; /* filter has more levels than the topic */
        if (flen == 1 && fl[0] == '+') continue; /* '+' = any one level */
        if (flen != tlen || memcmp(fl, tl, flen) != 0) return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Bounded queue (one mutex per queue)                                 */
/* ------------------------------------------------------------------ */

int bridge_queue_init(bridge_queue_t *q)
{
    if (!q) return -1;
    memset(q, 0, sizeof(*q));
    return pthread_mutex_init(&q->mutex, NULL) == 0 ? 0 : -1;
}

void bridge_queue_destroy(bridge_queue_t *q)
{
    if (!q) return;
    pthread_mutex_destroy(&q->mutex);
}

int bridge_queue_push(bridge_queue_t *q, const char *topic,
                      const void *payload, size_t payload_len)
{
    size_t topic_len;
    bridge_msg_t *slot;

    if (!q || !topic) return -1;
    topic_len = strlen(topic);
    if (topic_len == 0 || topic_len > QNX_BRIDGE_MAX_TOPIC_LEN) return -1;
    if (payload_len > QNX_BRIDGE_MAX_PAYLOAD_LEN) return -1;
    if (payload_len > 0 && !payload) return -1;

    pthread_mutex_lock(&q->mutex);
    if (q->count == QNX_BRIDGE_QUEUE_MAX) {
        /* Full: drop the OLDEST message, count it, keep the newest. */
        q->head = (q->head + 1) % QNX_BRIDGE_QUEUE_MAX;
        q->count--;
        q->dropped++;
    }
    slot = &q->msgs[q->tail];
    memcpy(slot->topic, topic, topic_len + 1);
    if (payload_len > 0) memcpy(slot->payload, payload, payload_len);
    slot->payload_len = payload_len;
    q->tail = (q->tail + 1) % QNX_BRIDGE_QUEUE_MAX;
    q->count++;
    pthread_mutex_unlock(&q->mutex);
    return 0;
}

int bridge_queue_pop(bridge_queue_t *q, bridge_msg_t *out)
{
    if (!q || !out) return -1;
    pthread_mutex_lock(&q->mutex);
    if (q->count == 0) {
        pthread_mutex_unlock(&q->mutex);
        return -1;
    }
    *out = q->msgs[q->head];
    q->head = (q->head + 1) % QNX_BRIDGE_QUEUE_MAX;
    q->count--;
    pthread_mutex_unlock(&q->mutex);
    return 0;
}

int bridge_queue_count(bridge_queue_t *q)
{
    int n;
    if (!q) return -1;
    pthread_mutex_lock(&q->mutex);
    n = q->count;
    pthread_mutex_unlock(&q->mutex);
    return n;
}

/* ------------------------------------------------------------------ */
/* Registry (worker thread only in v1; mutex kept for safety)          */
/* ------------------------------------------------------------------ */

void bridge_registry_init(bridge_registry_t *r)
{
    if (!r) return;
    memset(r, 0, sizeof(*r));
    pthread_mutex_init(&r->mutex, NULL);
}

void bridge_registry_destroy(bridge_registry_t *r)
{
    if (!r) return;
    pthread_mutex_destroy(&r->mutex);
}

int bridge_registry_register(bridge_registry_t *r, const char *channel_name,
                             const char *prefix, int coid)
{
    int i, free_slot = -1;

    if (!r || !channel_name || !*channel_name || !prefix || !*prefix
        || coid < 0)
        return -1;
    if (strlen(channel_name) > QNX_BRIDGE_MAX_CHANNEL_NAME
        || strlen(prefix) > QNX_BRIDGE_MAX_PREFIX_LEN)
        return -1;

    pthread_mutex_lock(&r->mutex);
    for (i = 0; i < QNX_BRIDGE_REGISTRY_MAX; i++) {
        if (r->entries[i].in_use
            && strcmp(r->entries[i].channel_name, channel_name) == 0) {
            /* Re-registration (app restarted): update coid + prefix. */
            r->entries[i].coid = coid;
            snprintf(r->entries[i].prefix,
                     sizeof(r->entries[i].prefix), "%s", prefix);
            pthread_mutex_unlock(&r->mutex);
            return 0;
        }
        if (!r->entries[i].in_use && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) {
        pthread_mutex_unlock(&r->mutex);
        return -1; /* registry full (max 8) */
    }
    snprintf(r->entries[free_slot].channel_name,
             sizeof(r->entries[free_slot].channel_name), "%s", channel_name);
    snprintf(r->entries[free_slot].prefix,
             sizeof(r->entries[free_slot].prefix), "%s", prefix);
    r->entries[free_slot].coid = coid;
    r->entries[free_slot].in_use = true;
    pthread_mutex_unlock(&r->mutex);
    return 0;
}

int bridge_registry_route(bridge_registry_t *r, const char *topic)
{
    int i, best_coid = -1;
    size_t best_len = 0;

    if (!r || !topic) return -1;
    pthread_mutex_lock(&r->mutex);
    for (i = 0; i < QNX_BRIDGE_REGISTRY_MAX; i++) {
        size_t plen;
        if (!r->entries[i].in_use) continue;
        plen = strlen(r->entries[i].prefix);
        if (plen > best_len && strncmp(topic, r->entries[i].prefix, plen) == 0) {
            best_len = plen;
            best_coid = r->entries[i].coid;
        }
    }
    pthread_mutex_unlock(&r->mutex);
    return best_coid;
}

/* ------------------------------------------------------------------ */
/* HELLO parsing                                                       */
/* ------------------------------------------------------------------ */

int bridge_hello_parse(const char *payload, size_t payload_len,
                       char *name_out, size_t name_cap,
                       char *prefix_out, size_t prefix_cap)
{
    char tmp[QNX_BRIDGE_MAX_CHANNEL_NAME + QNX_BRIDGE_MAX_PREFIX_LEN + 2];
    char *sp, *name, *prefix;
    size_t i, n;

    if (!payload || payload_len == 0 || !name_out || name_cap == 0
        || !prefix_out || prefix_cap == 0)
        return -1;
    if (payload_len >= sizeof(tmp)) return -1;
    memcpy(tmp, payload, payload_len);
    tmp[payload_len] = '\0';

    /* Split "<channel-name>[ <topic-prefix>]" on the first blank. */
    sp = tmp;
    while (*sp && *sp != ' ' && *sp != '\t') sp++;
    if (*sp) {
        *sp = '\0';
        prefix = sp + 1;
        while (*prefix == ' ' || *prefix == '\t') prefix++;
        if (!*prefix) prefix = NULL;
    } else {
        prefix = NULL;
    }
    name = tmp;
    if (!*name || strchr(name, '/') != NULL) return -1;
    if (strlen(name) >= name_cap) return -1;
    snprintf(name_out, name_cap, "%s", name);

    if (prefix) {
        if (strlen(prefix) >= prefix_cap) return -1;
        snprintf(prefix_out, prefix_cap, "%s", prefix);
        return 0;
    }

    /* No explicit prefix: derive "qnx/cmd/<name-minus-'qnx_'>/" with
     * '_' mapped to '/'. */
    n = strlen(name);
    {
        char derived[QNX_BRIDGE_MAX_PREFIX_LEN + 1];
        size_t o = 0;
        const char *body = name;
        const char *head = "qnx/cmd/";
        if (n > 4 && strncmp(name, "qnx_", 4) == 0) body = name + 4;
        if (strlen(head) + strlen(body) + 1 >= sizeof(derived)) return -1;
        memcpy(derived, head, strlen(head));
        o = strlen(head);
        for (i = 0; body[i]; i++)
            derived[o++] = (body[i] == '_') ? '/' : body[i];
        derived[o++] = '/';
        derived[o] = '\0';
        (void)n;
        if (strlen(derived) >= prefix_cap) return -1;
        snprintf(prefix_out, prefix_cap, "%s", derived);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Worker thread                                                       */
/* ------------------------------------------------------------------ */

/* Drain the outbound queue: pack each message, route it by longest
 * prefix, MsgSend it to the registered app channel. Runs ONLY in the
 * worker thread (called on every MsgReceive timeout tick). */
static void drain_outbound(bridge_state_t *st)
{
    bridge_msg_t msg;
    uint8_t frame[QNX_MSG_MAX_TOTAL];
    uint8_t reply[QNX_MSG_MAX_TOTAL];
    size_t frame_len, reply_len;

    while (bridge_queue_pop(&st->outbound, &msg) == 0) {
        int coid;
        if (qnx_msg_pack(msg.topic, msg.payload, msg.payload_len,
                         frame, sizeof(frame), &frame_len) != QNX_MSG_OK) {
            bridge_log(st, MOSQ_LOG_WARNING,
                       "drop outbound: cannot pack topic '%s'", msg.topic);
            st->route_failed++;
            continue;
        }
        coid = bridge_registry_route(&st->registry, msg.topic);
        if (coid < 0) {
            bridge_log(st, MOSQ_LOG_WARNING,
                       "drop outbound: no app registered for '%s'", msg.topic);
            st->route_failed++;
            continue;
        }
        if (qnx_ipc_send(coid, frame, frame_len,
                         reply, sizeof(reply), &reply_len) != QNX_IPC_OK) {
            bridge_log(st, MOSQ_LOG_WARNING,
                       "MsgSend to app failed for '%s'", msg.topic);
            st->route_failed++;
            continue;
        }
        st->routed++;
        if (reply_len > 0) {
            /* The app replies with a framed ack (topic qnx/sensor/ack
             * for the demo, qnx/sensor/status for ping). The ack is
             * the app's ONLY acknowledgement — it never MsgSends acks
             * separately — so enqueue it into the inbound queue for
             * the TICK to publish to MQTT. */
            char ack_topic[QNX_BRIDGE_MAX_TOPIC_LEN + 1];
            const uint8_t *ack_payload;
            size_t ack_len;
            if (qnx_msg_unpack(reply, reply_len, ack_topic,
                               sizeof(ack_topic), &ack_payload, &ack_len)
                == QNX_MSG_OK) {
                bridge_log(st, MOSQ_LOG_DEBUG,
                           "ack from app for '%s': %s", msg.topic, ack_topic);
                if (bridge_queue_push(&st->inbound, ack_topic,
                                      ack_payload, ack_len) != 0)
                    bridge_log(st, MOSQ_LOG_WARNING,
                               "drop ack: topic/payload too large ('%s')",
                               ack_topic);
            }
        }
    }
}

/* Handle one real MsgReceive message from a QNX app. Replies with a
 * 1-byte status (0 = ok, 1 = rejected) so the sender's MsgSend returns. */
static void handle_incoming(bridge_state_t *st, const uint8_t *buf,
                            size_t len, int rcvid)
{
    char topic[QNX_BRIDGE_MAX_TOPIC_LEN + 1];
    const uint8_t *payload;
    size_t payload_len;
    uint8_t status = 0;
    unsigned long dropped_before;

    if (qnx_msg_unpack(buf, len, topic, sizeof(topic),
                       &payload, &payload_len) != QNX_MSG_OK) {
        bridge_log(st, MOSQ_LOG_WARNING,
                   "drop inbound: bad framing (%zu bytes)", len);
        status = 1;
        goto reply;
    }

    if (strcmp(topic, QNX_BRIDGE_HELLO_TOPIC) == 0) {
        char name[QNX_BRIDGE_MAX_CHANNEL_NAME + 1];
        char prefix[QNX_BRIDGE_MAX_PREFIX_LEN + 1];
        int coid;
        if (bridge_hello_parse((const char *)payload, payload_len,
                               name, sizeof(name),
                               prefix, sizeof(prefix)) != 0) {
            bridge_log(st, MOSQ_LOG_WARNING, "drop HELLO: unparsable");
            status = 1;
            goto reply;
        }
        if (qnx_ipc_lookup(name, &coid) != QNX_IPC_OK) {
            bridge_log(st, MOSQ_LOG_WARNING,
                       "drop HELLO: cannot open channel '%s'", name);
            status = 1;
            goto reply;
        }
        if (bridge_registry_register(&st->registry, name, prefix, coid) != 0) {
            bridge_log(st, MOSQ_LOG_WARNING,
                       "drop HELLO: registry full for '%s'", name);
            status = 1;
            goto reply;
        }
        bridge_log(st, MOSQ_LOG_INFO,
                   "app registered: '%s' prefix '%s'", name, prefix);
        goto reply;
    }

    dropped_before = st->inbound.dropped;
    if (bridge_queue_push(&st->inbound, topic, payload, payload_len) != 0) {
        bridge_log(st, MOSQ_LOG_WARNING,
                   "drop inbound: topic/payload too large ('%s')", topic);
        status = 1;
        goto reply;
    }
    if (st->inbound.dropped != dropped_before)
        bridge_log(st, MOSQ_LOG_WARNING,
                   "inbound queue full: dropped oldest message");

reply:
    if (qnx_ipc_reply(rcvid, &status, 1) != QNX_IPC_OK)
        bridge_log(st, MOSQ_LOG_WARNING, "MsgReply failed (rcvid %d)", rcvid);
}

/* Ticker thread (v7): TimerTimeout with SIGEV_UNBLOCK did not wake
 * MsgReceive in the v6 QEMU run (the probe program blocked ~350 s
 * after a successful TimerTimeout), so the worker can no longer rely
 * on the timeout to wake up and drain the outbound queue. This thread
 * pulses the worker's channel every 100 ms; the worker drains the
 * outbound queue on every pulse (see bridge_worker_main). Pulses use
 * the side-channel coid, which is safe to use from any thread.
 * The armed TimerTimeout is kept as a second wake source: if it fires,
 * the worker drains on the TIMEOUT branch as before. */
static void *bridge_ticker_main(void *arg)
{
    bridge_state_t *st = (bridge_state_t *)arg;
    while (!atomic_load(&st->stop)) {
        usleep(100 * 1000);
        if (atomic_load(&st->stop)) break;
        (void)qnx_ipc_wake(st->channel);
    }
    return NULL;
}

void *bridge_worker_main(void *arg)
{
    bridge_state_t *st = (bridge_state_t *)arg;
    uint8_t buf[QNX_MSG_MAX_TOTAL];

    while (!atomic_load(&st->stop)) {
        size_t len = 0;
        int rcvid = 0;
        int rc = qnx_ipc_receive(st->channel, buf, sizeof(buf),
                                 &len, &rcvid,
                                 QNX_BRIDGE_WORKER_TIMEOUT_MS);
        if (rc == QNX_IPC_TIMEOUT) {
            /* No app message within 100 ms: flush MQTT->QNX commands. */
            drain_outbound(st);
            continue;
        }
        if (rc == QNX_IPC_PULSE) {
            /* Wake/ticker pulse: flush MQTT->QNX commands too (v7).
             * The 100 ms ticker guarantees this runs regularly even
             * when no app message arrives and the TimerTimeout does
             * not fire — a pulse that only re-checks the stop flag
             * would leave queued commands undelivered. */
            drain_outbound(st);
            continue;
        }
        if (rc != QNX_IPC_OK) {
            bridge_log(st, MOSQ_LOG_WARNING, "MsgReceive error, retrying");
            usleep(10 * 1000); /* avoid a tight error spin */
            continue;
        }
        handle_incoming(st, buf, len, rcvid);
    }
    /* Final flush so no queued command is lost on a clean shutdown. */
    drain_outbound(st);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Broker hooks (broker thread: queues only, NEVER blocking IPC)       */
/* ------------------------------------------------------------------ */

static bridge_state_t *state_from_userdata(void *userdata)
{
    if (userdata) return (bridge_state_t *)userdata;
    return g_state;
}

static int on_message(int event, void *event_data, void *userdata)
{
    struct mosquitto_evt_message *msg = (struct mosquitto_evt_message *)event_data;
    bridge_state_t *st = state_from_userdata(userdata);
    unsigned long dropped_before;

    (void)event;
    if (!st || !msg || !msg->topic) return MOSQ_ERR_SUCCESS;
    if (!bridge_topic_matches(st->cmd_topic, msg->topic))
        return MOSQ_ERR_SUCCESS; /* not a QNX command: ignore */
    if (msg->payloadlen > QNX_BRIDGE_MAX_PAYLOAD_LEN) {
        bridge_log(st, MOSQ_LOG_WARNING,
                   "drop MQTT msg: payload %u bytes too large ('%s')",
                   msg->payloadlen, msg->topic);
        return MOSQ_ERR_SUCCESS;
    }
    dropped_before = st->outbound.dropped;
    if (bridge_queue_push(&st->outbound, msg->topic, msg->payload,
                          msg->payloadlen) != 0) {
        bridge_log(st, MOSQ_LOG_WARNING,
                   "drop MQTT msg: bad topic/payload ('%s')", msg->topic);
        return MOSQ_ERR_SUCCESS;
    }
    if (st->outbound.dropped != dropped_before)
        bridge_log(st, MOSQ_LOG_WARNING,
                   "outbound queue full: dropped oldest message");
    return MOSQ_ERR_SUCCESS;
}

static int on_tick(int event, void *event_data, void *userdata)
{
    bridge_state_t *st = state_from_userdata(userdata);
    bridge_msg_t msg;

    (void)event;
    (void)event_data;
    if (!st) return MOSQ_ERR_SUCCESS;

    /* Drain the inbound queue; each message is published to MQTT.
     * mosquitto_broker_publish() takes ownership of a heap payload on
     * success and frees it, so pass a fresh heap copy per message and
     * free it ourselves only when the call fails. */
    while (bridge_queue_pop(&st->inbound, &msg) == 0) {
        void *heap = NULL;
        int rc;
        if (msg.payload_len > 0) {
            heap = malloc(msg.payload_len);
            if (!heap) {
                bridge_log(st, MOSQ_LOG_ERR, "out of memory in TICK");
                st->publish_failed++;
                continue;
            }
            memcpy(heap, msg.payload, msg.payload_len);
        }
        rc = mosquitto_broker_publish(NULL, msg.topic,
                                      (int)msg.payload_len, heap,
                                      0, false, NULL);
        if (rc != MOSQ_ERR_SUCCESS) {
            bridge_log(st, MOSQ_LOG_WARNING,
                       "publish failed for '%s' (rc=%d)", msg.topic, rc);
            free(heap);
            st->publish_failed++;
        } else {
            st->published++;
        }
    }
    return MOSQ_ERR_SUCCESS;
}

static int on_disconnect(int event, void *event_data, void *userdata)
{
    bridge_state_t *st = state_from_userdata(userdata);
    (void)event;
    (void)event_data;
    /* Not used in v1 (DESIGN.md): log only, no action. */
    bridge_log(st, MOSQ_LOG_DEBUG, "client disconnected");
    return MOSQ_ERR_SUCCESS;
}

/* NOTE: MOSQ_EVT_BASIC_AUTH is deliberately NOT registered. Registering
 * it would make this plugin an auth plugin: its return value would
 * decide whether clients may log in. v1 has no auth (see DESIGN.md
 * non-goals), so the event is left entirely to the broker. */

/* ------------------------------------------------------------------ */
/* Plugin entry points (mosquitto_plugin.h)                            */
/* ------------------------------------------------------------------ */

int mosquitto_plugin_version(int supported_version_count,
                             const int *supported_versions)
{
    int i;
    for (i = 0; i < supported_version_count; i++) {
        if (supported_versions[i] == 5) return 5;
    }
    return -1;
}

static void apply_option(bridge_state_t *st, const char *key,
                         const char *value)
{
    /* mosquitto passes plugin_opt_<key> with the "plugin_opt_" prefix
     * stripped; accept the prefixed form too, defensively. */
    if (!key || !value) return;
    if (strcmp(key, "qnx_cmd_topic") == 0
        || strcmp(key, "plugin_opt_qnx_cmd_topic") == 0) {
        snprintf(st->cmd_topic, sizeof(st->cmd_topic), "%s", value);
    } else if (strcmp(key, "qnx_bridge_name") == 0
               || strcmp(key, "plugin_opt_qnx_bridge_name") == 0) {
        snprintf(st->bridge_name, sizeof(st->bridge_name), "%s", value);
    } else if (strcmp(key, "qnx_log") == 0
               || strcmp(key, "plugin_opt_qnx_log") == 0) {
        st->log_enabled = !(strcmp(value, "0") == 0
                            || strcmp(value, "false") == 0
                            || strcmp(value, "no") == 0);
    }
}

int mosquitto_plugin_init(mosquitto_plugin_id_t *identifier, void **userdata,
                          struct mosquitto_opt *options, int option_count)
{
    bridge_state_t *st;
    int i, rc;

    if (!identifier || !userdata) return 1;
    *userdata = NULL;

    st = calloc(1, sizeof(*st));
    if (!st) return 1;
    snprintf(st->cmd_topic, sizeof(st->cmd_topic), "%s",
             QNX_BRIDGE_DEFAULT_CMD_TOPIC);
    snprintf(st->bridge_name, sizeof(st->bridge_name), "%s",
             QNX_BRIDGE_DEFAULT_NAME);
    st->log_enabled = true;
    st->identifier = NULL;
    atomic_init(&st->stop, false);

    for (i = 0; i < option_count && options; i++)
        apply_option(st, options[i].key, options[i].value);

    if (bridge_queue_init(&st->outbound) != 0
        || bridge_queue_init(&st->inbound) != 0) {
        free(st);
        return 1;
    }
    bridge_registry_init(&st->registry);

    if (qnx_ipc_create(st->bridge_name, &st->channel) != QNX_IPC_OK) {
        bridge_log(st, MOSQ_LOG_ERR, "cannot create channel '%s'",
                   st->bridge_name);
        bridge_queue_destroy(&st->outbound);
        bridge_queue_destroy(&st->inbound);
        bridge_registry_destroy(&st->registry);
        free(st);
        return 1;
    }

    st->identifier = identifier;
    rc = pthread_create(&st->worker, NULL, bridge_worker_main, st);
    if (rc != 0) {
        bridge_log(st, MOSQ_LOG_ERR, "cannot start worker thread");
        qnx_ipc_destroy(st->channel);
        bridge_queue_destroy(&st->outbound);
        bridge_queue_destroy(&st->inbound);
        bridge_registry_destroy(&st->registry);
        free(st);
        return 1;
    }
    st->worker_started = true;

    /* Ticker thread (v7): pulses the worker channel every 100 ms so
     * queued MQTT->QNX commands are drained even when the TimerTimeout
     * does not wake MsgReceive (v6 evidence). */
    rc = pthread_create(&st->ticker, NULL, bridge_ticker_main, st);
    if (rc != 0) {
        bridge_log(st, MOSQ_LOG_ERR, "cannot start ticker thread");
        atomic_store(&st->stop, true);
        qnx_ipc_wake(st->channel);
        pthread_join(st->worker, NULL);
        qnx_ipc_destroy(st->channel);
        bridge_queue_destroy(&st->outbound);
        bridge_queue_destroy(&st->inbound);
        bridge_registry_destroy(&st->registry);
        free(st);
        return 1;
    }
    st->ticker_started = true;

    rc = mosquitto_callback_register(identifier, MOSQ_EVT_MESSAGE,
                                     on_message, NULL, st);
    if (rc != MOSQ_ERR_SUCCESS) goto fail_register;
    rc = mosquitto_callback_register(identifier, MOSQ_EVT_TICK,
                                     on_tick, NULL, st);
    if (rc != MOSQ_ERR_SUCCESS) goto fail_register;
    rc = mosquitto_callback_register(identifier, MOSQ_EVT_DISCONNECT,
                                     on_disconnect, NULL, st);
    if (rc != MOSQ_ERR_SUCCESS) goto fail_register;

    *userdata = st;
    g_state = st;
    bridge_log(st, MOSQ_LOG_NOTICE,
               "qnx-mqtt-gateway started: bridge='%s' cmd_topic='%s'",
               st->bridge_name, st->cmd_topic);
    return 0;

fail_register:
    bridge_log(st, MOSQ_LOG_ERR, "cannot register broker callbacks (rc=%d)", rc);
    atomic_store(&st->stop, true);
    qnx_ipc_wake(st->channel);
    pthread_join(st->worker, NULL);
    if (st->ticker_started) pthread_join(st->ticker, NULL);
    qnx_ipc_destroy(st->channel);
    bridge_queue_destroy(&st->outbound);
    bridge_queue_destroy(&st->inbound);
    bridge_registry_destroy(&st->registry);
    free(st);
    return 1;
}

int mosquitto_plugin_cleanup(void *userdata, struct mosquitto_opt *options,
                             int option_count)
{
    bridge_state_t *st = (bridge_state_t *)userdata;

    (void)options;
    (void)option_count;
    if (!st) return 0;

    /* Unregister first so no hook can touch the state while we tear
     * it down (best effort: ignore errors, we are shutting down). */
    if (st->identifier) {
        mosquitto_callback_unregister((mosquitto_plugin_id_t *)st->identifier, MOSQ_EVT_MESSAGE,
                                      on_message, NULL);
        mosquitto_callback_unregister((mosquitto_plugin_id_t *)st->identifier, MOSQ_EVT_TICK,
                                      on_tick, NULL);
        mosquitto_callback_unregister((mosquitto_plugin_id_t *)st->identifier, MOSQ_EVT_DISCONNECT,
                                      on_disconnect, NULL);
    }

    /* Clean shutdown: set stop, pulse the worker out of MsgReceive,
     * join worker + ticker, then release the channel and the queues.
     * The ticker is joined BEFORE the channel is destroyed so its
     * final wake pulse still has a live channel to pulse. */
    atomic_store(&st->stop, true);
    if (st->channel) qnx_ipc_wake(st->channel);
    if (st->worker_started) pthread_join(st->worker, NULL);
    if (st->ticker_started) pthread_join(st->ticker, NULL);
    if (st->channel) qnx_ipc_destroy(st->channel);
    bridge_queue_destroy(&st->outbound);
    bridge_queue_destroy(&st->inbound);
    bridge_registry_destroy(&st->registry);
    if (g_state == st) g_state = NULL;
    bridge_log(st, MOSQ_LOG_NOTICE,
               "qnx-mqtt-gateway stopped: published=%lu routed=%lu "
               "publish_failed=%lu route_failed=%lu",
               st->published, st->routed,
               st->publish_failed, st->route_failed);
    free(st);
    return 0;
}

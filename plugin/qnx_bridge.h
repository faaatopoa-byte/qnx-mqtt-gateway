/*
 * qnx_bridge.h — Mosquitto v5 plugin bridging MQTT topics to QNX native
 * message passing (see DESIGN.md).
 *
 * Data flow:
 *   MQTT --(MOSQ_EVT_MESSAGE, topic qnx/cmd/#)--> outbound queue
 *        --(worker thread, MsgSend)--> QNX app channel (registered via HELLO)
 *   QNX app --(MsgSend to qnx_mqtt_bridge)--> inbound queue
 *        --(MOSQ_EVT_TICK, mosquitto_broker_publish)--> MQTT
 *
 * Threading (DESIGN.md): the broker is a single-threaded poll() loop, so
 * ALL blocking QNX IPC runs in the plugin's own worker thread, never in a
 * hook. Hooks only touch the queues (each queue has its own mutex); the
 * worker is the only thread that touches the registry and the IPC channel.
 * No condition variable is used: TICK polls the inbound queue; the worker
 * is woken every 100 ms by a dedicated ticker thread (MsgSendPulse on the
 * side-channel coid) and drains the outbound queue on every wake
 * (v7 — TimerTimeout with SIGEV_UNBLOCK did not wake MsgReceive in the
 * v6 QEMU run, so the timeout alone is no longer trusted as the tick).
 *
 * The queue / registry / topic-match helpers are exposed (non-static) so
 * test/test_gateway.c can unit-test them without a broker.
 */
#ifndef QNX_BRIDGE_H
#define QNX_BRIDGE_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#include "qnx_ipc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define QNX_BRIDGE_QUEUE_MAX        64    /* bounded queue, DESIGN.md */
#define QNX_BRIDGE_REGISTRY_MAX     8     /* max registered QNX apps */
#define QNX_BRIDGE_MAX_TOPIC_LEN    255   /* matches QNX_MSG_MAX_TOPIC_LEN */
#define QNX_BRIDGE_MAX_PAYLOAD_LEN  3840  /* 4096 - header - max topic, rounded down */
#define QNX_BRIDGE_MAX_CHANNEL_NAME 63
#define QNX_BRIDGE_MAX_PREFIX_LEN   127
#define QNX_BRIDGE_WORKER_TIMEOUT_MS 100  /* TimerTimeout / MsgReceive budget */
#define QNX_BRIDGE_HELLO_TOPIC      "__hello__"

#define QNX_BRIDGE_DEFAULT_CMD_TOPIC "qnx/cmd/#"
#define QNX_BRIDGE_DEFAULT_NAME      "qnx_mqtt_bridge"

/* One queued message (either direction). Topic is NUL-terminated;
 * payload is raw bytes of payload_len. */
typedef struct {
    char topic[QNX_BRIDGE_MAX_TOPIC_LEN + 1];
    uint8_t payload[QNX_BRIDGE_MAX_PAYLOAD_LEN];
    size_t payload_len;
} bridge_msg_t;

/* Bounded ring queue. Overflow drops the OLDEST message and counts it.
 * Each queue has its own mutex; push/pop are safe from any thread. */
typedef struct {
    bridge_msg_t msgs[QNX_BRIDGE_QUEUE_MAX];
    int head;             /* index of oldest message */
    int tail;             /* index where the next message goes */
    int count;
    unsigned long dropped; /* messages dropped by overflow */
    pthread_mutex_t mutex;
} bridge_queue_t;

int  bridge_queue_init(bridge_queue_t *q);
void bridge_queue_destroy(bridge_queue_t *q);
/* Returns 0 on success; always succeeds (overflow drops oldest instead). */
int  bridge_queue_push(bridge_queue_t *q, const char *topic,
                       const void *payload, size_t payload_len);
/* Returns 0 and fills *out, or -1 when the queue is empty. */
int  bridge_queue_pop(bridge_queue_t *q, bridge_msg_t *out);
int  bridge_queue_count(bridge_queue_t *q);

/* One registered QNX app: its channel name, the coid to MsgSend to it,
 * and the MQTT topic prefix it serves (e.g. "qnx/cmd/sensor/"). */
typedef struct {
    char channel_name[QNX_BRIDGE_MAX_CHANNEL_NAME + 1];
    char prefix[QNX_BRIDGE_MAX_PREFIX_LEN + 1];
    int coid;
    bool in_use;
} bridge_registry_entry_t;

typedef struct {
    bridge_registry_entry_t entries[QNX_BRIDGE_REGISTRY_MAX];
    pthread_mutex_t mutex;
} bridge_registry_t;

void bridge_registry_init(bridge_registry_t *r);
void bridge_registry_destroy(bridge_registry_t *r);
/* Register (or re-register, updating the coid) an app. Returns 0 on
 * success, -1 when the registry is full (max 8) or args are bad. */
int  bridge_registry_register(bridge_registry_t *r, const char *channel_name,
                              const char *prefix, int coid);
/* Longest-prefix routing: returns the coid for the registered prefix
 * that is the longest match for topic, or -1 when no prefix matches. */
int  bridge_registry_route(bridge_registry_t *r, const char *topic);

/*
 * Parse a HELLO payload (topic "__hello__", sent by a QNX app on start).
 * Payload form: "<channel-name>" or "<channel-name> <topic-prefix>".
 * With no explicit prefix, the default prefix is derived as
 * "qnx/cmd/<channel-name-minus-leading-'qnx_'>/" with '_' mapped to '/'
 * (e.g. "qnx_demo_sensor" -> "qnx/cmd/demo/sensor/"). The demo app sends
 * an explicit prefix ("qnx_demo_sensor qnx/cmd/sensor/"), so the
 * derivation is only a fallback for apps that do not name one.
 * Returns 0 on success, -1 on bad args / unparsable payload.
 */
int bridge_hello_parse(const char *payload, size_t payload_len,
                       char *name_out, size_t name_cap,
                       char *prefix_out, size_t prefix_cap);

/*
 * MQTT topic filter match (subset of MQTT wildcards, enough for v1):
 *  - exact match;
 *  - trailing "/#" matches the base topic and everything below it
 *    ("qnx/cmd/#" matches "qnx/cmd" and "qnx/cmd/sensor/x");
 *  - "+" matches exactly one level ("qnx/+/temp" matches
 *    "qnx/sensor/temp" but not "qnx/a/b/temp").
 * Returns 1 on match, 0 otherwise.
 */
int bridge_topic_matches(const char *filter, const char *topic);

/* Plugin runtime state (one instance; owned by mosquitto_plugin_init). */
typedef struct bridge_state {
    char cmd_topic[QNX_BRIDGE_MAX_TOPIC_LEN + 1]; /* MQTT->QNX filter */
    char bridge_name[QNX_BRIDGE_MAX_CHANNEL_NAME + 1];
    bool log_enabled;
    bridge_queue_t outbound; /* MQTT -> QNX (filled by MESSAGE hook) */
    bridge_queue_t inbound;  /* QNX -> MQTT (drained by TICK hook) */
    bridge_registry_t registry;
    qnx_ipc_channel_t *channel; /* "qnx_mqtt_bridge", owned by worker */
    void *identifier;    /* mosquitto_plugin_id_t *, opaque here (for unregister) */
    pthread_t worker;
    bool worker_started;
    pthread_t ticker;    /* 100 ms pulse ticker (v7); pulses worker channel */
    bool ticker_started;
    atomic_bool stop;        /* set by cleanup, read by worker */
    unsigned long published; /* inbound messages published on TICK */
    unsigned long publish_failed;
    unsigned long routed;    /* outbound messages MsgSend to an app */
    unsigned long route_failed;
} bridge_state_t;

/* Worker-thread entry point (also callable from tests with a stub
 * channel). Exposed for the host tests only; not part of the plugin API. */
void *bridge_worker_main(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* QNX_BRIDGE_H */

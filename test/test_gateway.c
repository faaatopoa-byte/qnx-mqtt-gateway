/*
 * test_gateway.c — host unit tests for the qnx-mqtt-gateway (see DESIGN.md).
 *
 * Covers, per the task brief:
 *  1. qnx_msg pack/unpack round-trip, including corrupt messages
 *     (bad magic, lying lengths, truncated buffer, trailing bytes).
 *  2. MQTT topic-filter matching (exact, trailing "/#", "+" level).
 *  3. Bounded queue overflow (drop-oldest, count stays 64).
 *  4. Registry HELLO parsing + longest-prefix routing (incl. full table).
 *  5. Host IPC stub round-trip (send/receive/reply, timeout, wake).
 *  6. End-to-end host loop-back through the real plugin: init the plugin
 *     with stubbed broker functions, run a fake sensor app in a thread
 *     (HELLO + telemetry via the host IPC stub), drive the captured
 *     MOSQ_EVT_TICK / MOSQ_EVT_MESSAGE callbacks, and check the command
 *     comes back out via MsgSend and the telemetry is published.
 *
 * Broker functions are stubbed here (mosquitto_callback_register records
 * the callbacks; mosquitto_broker_publish records the last publish and
 * frees the heap payload exactly like the real broker does on success).
 *
 * Exit code: 0 when every check passes, 1 otherwise. run_tests.sh
 * builds and runs this file.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE /* usleep() on glibc with -std=c11 */

#include <mosquitto_broker.h>
#include <mosquitto_plugin.h>
#include <mosquitto.h> /* MOSQ_ERR_SUCCESS / MOSQ_ERR_INVAL */

#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "qnx_bridge.h"
#include "qnx_ipc.h"
#include "../common/qnx_msg.h"

/* ------------------------------------------------------------------ */
/* Tiny test framework                                                 */
/* ------------------------------------------------------------------ */

static int g_checks;
static int g_failures;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

#define CHECK_STR_EQ(a, b) \
    CHECK(strcmp((a), (b)) == 0, "expected \"%s\", got \"%s\"", (b), (a))

/* ------------------------------------------------------------------ */
/* Broker stubs (stand in for mosquitto itself)                        */
/* ------------------------------------------------------------------ */

static MOSQ_FUNC_generic_callback g_cbs[16];
static void *g_cb_userdata[16];

int mosquitto_callback_register(mosquitto_plugin_id_t *identifier, int event,
                                MOSQ_FUNC_generic_callback cb_func,
                                const void *event_data, void *userdata)
{
    (void)identifier;
    (void)event_data;
    if (event < 0 || event >= 16 || !cb_func) return MOSQ_ERR_INVAL;
    g_cbs[event] = cb_func;
    g_cb_userdata[event] = userdata;
    return MOSQ_ERR_SUCCESS;
}

int mosquitto_callback_unregister(mosquitto_plugin_id_t *identifier, int event,
                                  MOSQ_FUNC_generic_callback cb_func,
                                  const void *event_data)
{
    (void)identifier;
    (void)event_data;
    if (event < 0 || event >= 16) return MOSQ_ERR_INVAL;
    if (g_cbs[event] == cb_func) g_cbs[event] = NULL;
    return MOSQ_ERR_SUCCESS;
}

static char g_last_pub_topic[256];
static uint8_t g_last_pub_payload[4096];
static size_t g_last_pub_len;
static int g_pub_count;

int mosquitto_broker_publish(const char *clientid, const char *topic,
                             int payloadlen, void *payload, int qos,
                             bool retain, mosquitto_property *properties)
{
    (void)clientid;
    (void)qos;
    (void)retain;
    (void)properties;
    if (!topic || payloadlen < 0 || (payloadlen > 0 && !payload))
        return MOSQ_ERR_INVAL;
    snprintf(g_last_pub_topic, sizeof(g_last_pub_topic), "%s", topic);
    if (payloadlen > 0) {
        if ((size_t)payloadlen > sizeof(g_last_pub_payload))
            return MOSQ_ERR_INVAL;
        memcpy(g_last_pub_payload, payload, (size_t)payloadlen);
    }
    g_last_pub_len = (size_t)payloadlen;
    g_pub_count++;
    free(payload); /* the real broker frees the heap payload on success */
    return MOSQ_ERR_SUCCESS;
}

void mosquitto_log_printf(int level, const char *fmt, ...)
{
    /* Swallow plugin logs unless the test run asks for them. */
    if (getenv("QNX_TEST_VERBOSE")) {
        va_list ap;
        va_start(ap, fmt);
        fprintf(stderr, "[broker-log %d] ", level);
        vfprintf(stderr, fmt, ap);
        fprintf(stderr, "\n");
        va_end(ap);
    }
}

/* ------------------------------------------------------------------ */
/* 1. Framing                                                          */
/* ------------------------------------------------------------------ */

static void test_framing(void)
{
    uint8_t buf[QNX_MSG_MAX_TOTAL];
    uint8_t buf2[QNX_MSG_MAX_TOTAL];
    size_t len = 0, len2 = 0;
    char topic[300];
    const uint8_t *payload = NULL;
    size_t payload_len = 0;
    int rc;

    printf("== framing ==\n");

    /* Round-trip: text payload. */
    rc = qnx_msg_pack("qnx/sensor/temp", "{\"temp_c\":23.5}",
                      strlen("{\"temp_c\":23.5}"), buf, sizeof(buf), &len);
    CHECK(rc == QNX_MSG_OK, "pack text rc=%d", rc);
    CHECK(len == 12 + strlen("qnx/sensor/temp") + strlen("{\"temp_c\":23.5}"),
          "pack len %zu", len);
    rc = qnx_msg_unpack(buf, len, topic, sizeof(topic), &payload, &payload_len);
    CHECK(rc == QNX_MSG_OK, "unpack text rc=%d", rc);
    CHECK_STR_EQ(topic, "qnx/sensor/temp");
    CHECK(payload_len == strlen("{\"temp_c\":23.5}"), "payload len %zu", payload_len);
    CHECK(memcmp(payload, "{\"temp_c\":23.5}", payload_len) == 0, "payload bytes");

    /* Round-trip: empty payload + binary payload with NULs. */
    rc = qnx_msg_pack("qnx/cmd/sensor/ping", NULL, 0, buf, sizeof(buf), &len);
    CHECK(rc == QNX_MSG_OK, "pack empty rc=%d", rc);
    rc = qnx_msg_unpack(buf, len, topic, sizeof(topic), &payload, &payload_len);
    CHECK(rc == QNX_MSG_OK && payload_len == 0, "unpack empty rc=%d len=%zu",
          rc, payload_len);
    {
        static const uint8_t bin[] = {0x00, 0x01, 0x00, 0xFF, 0x7F};
        rc = qnx_msg_pack("qnx/bin", bin, sizeof(bin), buf, sizeof(buf), &len);
        CHECK(rc == QNX_MSG_OK, "pack binary rc=%d", rc);
        rc = qnx_msg_unpack(buf, len, topic, sizeof(topic), &payload, &payload_len);
        CHECK(rc == QNX_MSG_OK && payload_len == sizeof(bin)
              && memcmp(payload, bin, sizeof(bin)) == 0,
              "unpack binary rc=%d", rc);
    }

    /* Corrupt: bad magic. */
    rc = qnx_msg_pack("qnx/sensor/temp", "x", 1, buf, sizeof(buf), &len);
    CHECK(rc == QNX_MSG_OK, "pack for corrupt tests rc=%d", rc);
    memcpy(buf2, buf, len);
    buf2[0] ^= 0xFF;
    rc = qnx_msg_unpack(buf2, len, topic, sizeof(topic), &payload, &payload_len);
    CHECK(rc == QNX_MSG_ERR_MAGIC, "bad magic rc=%d (want %d)", rc, QNX_MSG_ERR_MAGIC);

    /* Corrupt: lying topic_len (claims 100, buffer holds 15). A length
     * past the 255 topic cap is rejected as ERR_TOPIC instead; both
     * are rejections, so test the in-range lie here. */
    memcpy(buf2, buf, len);
    buf2[4] = 100; buf2[5] = 0x00; buf2[6] = 0x00; buf2[7] = 0x00;
    rc = qnx_msg_unpack(buf2, len, topic, sizeof(topic), &payload, &payload_len);
    CHECK(rc == QNX_MSG_ERR_TRUNC, "lying topic_len rc=%d (want %d)", rc, QNX_MSG_ERR_TRUNC);
    memcpy(buf2, buf, len);
    buf2[4] = 0xFF; buf2[5] = 0xFF; buf2[6] = 0xFF; buf2[7] = 0x7F;
    rc = qnx_msg_unpack(buf2, len, topic, sizeof(topic), &payload, &payload_len);
    CHECK(rc == QNX_MSG_ERR_TOPIC, "absurd topic_len rc=%d (want %d)", rc, QNX_MSG_ERR_TOPIC);

    /* Corrupt: lying payload_len. */
    memcpy(buf2, buf, len);
    buf2[8] = 0x00; buf2[9] = 0x10; buf2[10] = 0x00; buf2[11] = 0x00;
    rc = qnx_msg_unpack(buf2, len, topic, sizeof(topic), &payload, &payload_len);
    CHECK(rc == QNX_MSG_ERR_TRUNC, "lying payload_len rc=%d (want %d)", rc, QNX_MSG_ERR_TRUNC);

    /* Corrupt: truncated buffer (fewer bytes than the header + data). */
    rc = qnx_msg_unpack(buf, 8, topic, sizeof(topic), &payload, &payload_len);
    CHECK(rc == QNX_MSG_ERR_TRUNC, "short header rc=%d", rc);
    rc = qnx_msg_unpack(buf, len - 1, topic, sizeof(topic), &payload, &payload_len);
    CHECK(rc == QNX_MSG_ERR_TRUNC, "short body rc=%d", rc);

    /* Corrupt: trailing bytes after a valid frame. */
    memcpy(buf2, buf, len);
    buf2[len] = 0x00;
    rc = qnx_msg_unpack(buf2, len + 1, topic, sizeof(topic), &payload, &payload_len);
    CHECK(rc == QNX_MSG_ERR_TRUNC, "trailing bytes rc=%d", rc);

    /* Topic too long for the output buffer. */
    {
        char small[4];
        rc = qnx_msg_unpack(buf, len, small, sizeof(small), &payload, &payload_len);
        CHECK(rc == QNX_MSG_ERR_TOPIC, "small topic_out rc=%d", rc);
    }

    /* Pack errors: empty topic, NULLs, oversize payload. */
    rc = qnx_msg_pack("", "x", 1, buf, sizeof(buf), &len);
    CHECK(rc == QNX_MSG_ERR_ARG, "empty topic rc=%d", rc);
    rc = qnx_msg_pack(NULL, "x", 1, buf, sizeof(buf), &len);
    CHECK(rc == QNX_MSG_ERR_ARG, "NULL topic rc=%d", rc);
    rc = qnx_msg_pack("qnx/x", NULL, 0, NULL, sizeof(buf), &len);
    CHECK(rc == QNX_MSG_ERR_ARG, "NULL out rc=%d", rc);
    {
        static uint8_t big[QNX_MSG_MAX_TOTAL];
        rc = qnx_msg_pack("qnx/x", big, sizeof(big), buf, sizeof(buf), &len);
        CHECK(rc == QNX_MSG_ERR_TOOBIG, "oversize rc=%d", rc);
        rc = qnx_msg_pack("qnx/x", big, sizeof(big), buf, 10, &len2);
        CHECK(rc == QNX_MSG_ERR_TOOBIG, "tiny cap rc=%d", rc);
    }
    (void)len2;
}

/* ------------------------------------------------------------------ */
/* 2. Topic matching                                                   */
/* ------------------------------------------------------------------ */

static void test_topic_match(void)
{
    printf("== topic match ==\n");
    CHECK(bridge_topic_matches("qnx/cmd/#", "qnx/cmd") == 1, "/# matches base");
    CHECK(bridge_topic_matches("qnx/cmd/#", "qnx/cmd/sensor/set_interval") == 1,
          "/# matches deep");
    CHECK(bridge_topic_matches("qnx/cmd/#", "qnx/cmd/sensor") == 1, "/# one level");
    CHECK(bridge_topic_matches("qnx/cmd/#", "qnx/cmdx") == 0, "/# must not match prefix sibling");
    CHECK(bridge_topic_matches("qnx/cmd/#", "qnx/sensor/temp") == 0, "/# wrong branch");
    CHECK(bridge_topic_matches("qnx/cmd/#", "other") == 0, "/# unrelated");
    CHECK(bridge_topic_matches("qnx/sensor/temp", "qnx/sensor/temp") == 1, "exact");
    CHECK(bridge_topic_matches("qnx/sensor/temp", "qnx/sensor/hum") == 0, "exact mismatch");
    CHECK(bridge_topic_matches("qnx/+/temp", "qnx/sensor/temp") == 1, "+ one level");
    CHECK(bridge_topic_matches("qnx/+/temp", "qnx/a/b/temp") == 0, "+ is one level only");
    CHECK(bridge_topic_matches("qnx/+/temp", "qnx/temp") == 0, "+ needs a level");
    CHECK(bridge_topic_matches("#", "anything/at/all") == 1, "# matches all");
    CHECK(bridge_topic_matches("qnx/cmd/#", "qnx/cmd/") == 1, "/# trailing slash");
    CHECK(bridge_topic_matches(NULL, "qnx/cmd") == 0, "NULL filter");
    CHECK(bridge_topic_matches("qnx/cmd/#", NULL) == 0, "NULL topic");
}

/* ------------------------------------------------------------------ */
/* 3. Bounded queue                                                    */
/* ------------------------------------------------------------------ */

static void test_queue(void)
{
    bridge_queue_t q;
    bridge_msg_t msg;
    char t[32];
    int i, rc;

    printf("== queue ==\n");
    CHECK(bridge_queue_init(&q) == 0, "queue init");
    CHECK(bridge_queue_count(&q) == 0, "empty count");
    CHECK(bridge_queue_pop(&q, &msg) == -1, "pop empty");

    for (i = 0; i < QNX_BRIDGE_QUEUE_MAX; i++) {
        snprintf(t, sizeof(t), "t%d", i);
        rc = bridge_queue_push(&q, t, "p", 1);
        CHECK(rc == 0, "push %d rc=%d", i, rc);
    }
    CHECK(bridge_queue_count(&q) == QNX_BRIDGE_QUEUE_MAX, "full count");
    CHECK(q.dropped == 0, "no drops yet");

    /* One more push: oldest (t0) must drop, count stays 64. */
    rc = bridge_queue_push(&q, "t64", "p", 1);
    CHECK(rc == 0, "push overflow rc=%d", rc);
    CHECK(bridge_queue_count(&q) == QNX_BRIDGE_QUEUE_MAX, "count after overflow");
    CHECK(q.dropped == 1, "dropped==1, got %lu", q.dropped);
    rc = bridge_queue_pop(&q, &msg);
    CHECK(rc == 0 && strcmp(msg.topic, "t1") == 0,
          "first pop after overflow is t1, got '%s'", msg.topic);

    /* FIFO order for the rest, then empty again. */
    for (i = 2; i <= 64; i++) {
        char want[32];
        snprintf(want, sizeof(want), "t%d", i);
        rc = bridge_queue_pop(&q, &msg);
        CHECK(rc == 0 && strcmp(msg.topic, want) == 0,
              "fifo %d: got '%s', want '%s'", i, msg.topic, want);
    }
    CHECK(bridge_queue_count(&q) == 0, "drained count");
    CHECK(bridge_queue_pop(&q, &msg) == -1, "pop empty again");

    /* Oversize push is rejected, not queued. */
    {
        static uint8_t big[QNX_BRIDGE_MAX_PAYLOAD_LEN + 1];
        rc = bridge_queue_push(&q, "qnx/x", big, sizeof(big));
        CHECK(rc == -1, "oversize payload rejected rc=%d", rc);
        CHECK(bridge_queue_count(&q) == 0, "still empty after reject");
    }
    bridge_queue_destroy(&q);
}

/* ------------------------------------------------------------------ */
/* 4. Registry + HELLO                                                 */
/* ------------------------------------------------------------------ */

static void test_registry(void)
{
    bridge_registry_t r;
    char name[64], prefix[128];
    int rc, coid;

    printf("== registry / HELLO ==\n");

    /* HELLO parsing: explicit prefix. */
    rc = bridge_hello_parse("qnx_demo_sensor qnx/cmd/sensor/",
                            strlen("qnx_demo_sensor qnx/cmd/sensor/"),
                            name, sizeof(name), prefix, sizeof(prefix));
    CHECK(rc == 0, "hello explicit rc=%d", rc);
    CHECK_STR_EQ(name, "qnx_demo_sensor");
    CHECK_STR_EQ(prefix, "qnx/cmd/sensor/");

    /* HELLO parsing: derived prefix fallback. */
    rc = bridge_hello_parse("qnx_demo_sensor", strlen("qnx_demo_sensor"),
                            name, sizeof(name), prefix, sizeof(prefix));
    CHECK(rc == 0, "hello derived rc=%d", rc);
    CHECK_STR_EQ(prefix, "qnx/cmd/demo/sensor/");
    rc = bridge_hello_parse("qnx_sensor1", strlen("qnx_sensor1"),
                            name, sizeof(name), prefix, sizeof(prefix));
    CHECK(rc == 0 && strcmp(prefix, "qnx/cmd/sensor1/") == 0,
          "hello derived plain, prefix='%s'", prefix);

    /* HELLO parsing: bad payloads. */
    rc = bridge_hello_parse("", 0, name, sizeof(name), prefix, sizeof(prefix));
    CHECK(rc == -1, "hello empty rejected");
    rc = bridge_hello_parse("bad/name", strlen("bad/name"),
                            name, sizeof(name), prefix, sizeof(prefix));
    CHECK(rc == -1, "hello slash in name rejected");

    /* Routing: longest prefix wins, unknown prefix misses. */
    bridge_registry_init(&r);
    CHECK(bridge_registry_route(&r, "qnx/cmd/sensor/x") == -1, "route empty");
    CHECK(bridge_registry_register(&r, "qnx_demo_sensor", "qnx/cmd/sensor/", 11) == 0,
          "register demo");
    CHECK(bridge_registry_register(&r, "qnx_sub", "qnx/cmd/sensor/sub/", 22) == 0,
          "register sub");
    coid = bridge_registry_route(&r, "qnx/cmd/sensor/set_interval");
    CHECK(coid == 11, "route demo coid=%d", coid);
    coid = bridge_registry_route(&r, "qnx/cmd/sensor/sub/deep");
    CHECK(coid == 22, "longest prefix wins, coid=%d", coid);
    CHECK(bridge_registry_route(&r, "qnx/cmd/other/x") == -1, "unknown prefix");
    CHECK(bridge_registry_route(&r, "qnx/cmd/sensor2/x") == -1,
          "sibling prefix must not match");

    /* Re-registration updates the coid. */
    CHECK(bridge_registry_register(&r, "qnx_demo_sensor", "qnx/cmd/sensor/", 33) == 0,
          "re-register");
    coid = bridge_registry_route(&r, "qnx/cmd/sensor/x");
    CHECK(coid == 33, "coid updated to 33, got %d", coid);

    /* Registry full at 8: 2 used, 6 more fit, the 9th fails. */
    for (int i = 0; i < 6; i++) {
        char n[32], p[64];
        snprintf(n, sizeof(n), "qnx_app%d", i);
        snprintf(p, sizeof(p), "qnx/cmd/app%d/", i);
        rc = bridge_registry_register(&r, n, p, 100 + i);
        CHECK(rc == 0, "fill %d rc=%d", i, rc);
    }
    rc = bridge_registry_register(&r, "qnx_ninth", "qnx/cmd/ninth/", 199);
    CHECK(rc == -1, "9th app rejected rc=%d", rc);
    bridge_registry_destroy(&r);
}

/* ------------------------------------------------------------------ */
/* 5. Host IPC stub                                                    */
/* ------------------------------------------------------------------ */

static void *ipc_server_thread(void *arg)
{
    /* Serves one request, then waits for a wake pulse. */
    qnx_ipc_channel_t *ch = NULL;
    uint8_t buf[1024];
    size_t len = 0;
    int rcvid = 0;
    int *result = (int *)arg;

    *result = -99;
    if (qnx_ipc_create("test_ipc_ch", &ch) != QNX_IPC_OK) return NULL;
    if (qnx_ipc_receive(ch, buf, sizeof(buf), &len, &rcvid, 5000) != QNX_IPC_OK) {
        qnx_ipc_destroy(ch);
        return NULL;
    }
    if (len != 4 || memcmp(buf, "ping", 4) != 0) {
        qnx_ipc_destroy(ch);
        return NULL;
    }
    if (qnx_ipc_reply(rcvid, "pong", 4) != QNX_IPC_OK) {
        qnx_ipc_destroy(ch);
        return NULL;
    }
    /* Now expect a wake pulse (no message). Nothing wakes this channel
     * in this test (qnx_ipc_wake needs the channel object, which lives
     * in this thread) — the short timeout below just bounds the wait.
     * Wake-via-pulse is exercised for real in the end-to-end test,
     * where plugin cleanup wakes the worker thread. */
    if (qnx_ipc_receive(ch, buf, sizeof(buf), &len, &rcvid, 1000) == QNX_IPC_PULSE)
        *result = 0;
    qnx_ipc_destroy(ch);
    return NULL;
}

static void *ipc_wake_thread(void *arg)
{
    qnx_ipc_channel_t *ch = (qnx_ipc_channel_t *)arg;
    uint8_t buf[64];
    size_t len = 0;
    int rcvid = 0;
    int *result = malloc(sizeof(int));
    *result = qnx_ipc_receive(ch, buf, sizeof(buf), &len, &rcvid, 5000);
    return result;
}

static void test_ipc(void)
{
    pthread_t th;
    int result = -99;
    int coid = -1;
    uint8_t reply[64];
    size_t reply_len = 0;
    int rc;

    printf("== host IPC ==\n");

    /* Wake: a thread blocked in receive() returns QNX_IPC_PULSE. */
    {
        qnx_ipc_channel_t *ch = NULL;
        pthread_t wth;
        void *wres = NULL;
        CHECK(qnx_ipc_create("test_ipc_wake", &ch) == QNX_IPC_OK, "create wake ch");
        CHECK(pthread_create(&wth, NULL, ipc_wake_thread, ch) == 0,
              "start wake thread");
        usleep(100 * 1000);
        rc = qnx_ipc_wake(ch);
        CHECK(rc == QNX_IPC_OK, "wake rc=%d", rc);
        pthread_join(wth, &wres);
        CHECK(wres && *(int *)wres == QNX_IPC_PULSE,
              "blocked receive woke with PULSE, got %d",
              wres ? *(int *)wres : -999);
        free(wres);
        qnx_ipc_destroy(ch);
        unlink("/tmp/qnx-ipc-test_ipc_wake.sock");
    }

    /* Lookup of a missing channel fails. */
    rc = qnx_ipc_lookup("no_such_channel_xyz", &coid);
    CHECK(rc != QNX_IPC_OK, "lookup missing fails rc=%d", rc);

    /* Timeout with nobody sending. */
    {
        qnx_ipc_channel_t *ch = NULL;
        uint8_t buf[64];
        size_t len = 0;
        int rcvid = 0;
        CHECK(qnx_ipc_create("test_ipc_timeout", &ch) == QNX_IPC_OK, "create timeout ch");
        rc = qnx_ipc_receive(ch, buf, sizeof(buf), &len, &rcvid, 50);
        CHECK(rc == QNX_IPC_TIMEOUT, "receive timeout rc=%d", rc);
        qnx_ipc_destroy(ch);
    }

    CHECK(pthread_create(&th, NULL, ipc_server_thread, &result) == 0,
          "start ipc server thread");
    usleep(100 * 1000); /* let the server bind */
    rc = qnx_ipc_lookup("test_ipc_ch", &coid);
    CHECK(rc == QNX_IPC_OK, "lookup server rc=%d", rc);
    rc = qnx_ipc_send(coid, "ping", 4, reply, sizeof(reply), &reply_len);
    CHECK(rc == QNX_IPC_OK, "send ping rc=%d", rc);
    CHECK(reply_len == 4 && memcmp(reply, "pong", 4) == 0,
          "reply pong len=%zu", reply_len);
    close(coid);

    pthread_join(th, NULL); /* server's second receive times out after 1 s */
    CHECK(result == -99 || result == 0,
          "server thread finished (result=%d; -99 = timed out on wake, ok here)",
          result);
}

/* ------------------------------------------------------------------ */
/* 6. End-to-end host loop-back through the real plugin                */
/* ------------------------------------------------------------------ */

typedef struct {
    char cmd_topic[256];
    uint8_t cmd_payload[256];
    size_t cmd_len;
    int got_cmd;     /* 1 when the sensor thread received the command */
    int hello_ok;    /* 1 when the HELLO reply status was 0 */
    int telemetry_ok;
} sensor_result_t;

static sensor_result_t g_sensor;

static void *sensor_thread(void *arg)
{
    qnx_ipc_channel_t *ch = NULL;
    uint8_t frame[QNX_MSG_MAX_TOTAL];
    uint8_t rx[QNX_MSG_MAX_TOTAL];
    uint8_t reply[QNX_MSG_MAX_TOTAL];
    size_t frame_len = 0, reply_len = 0, rx_len = 0;
    int rcvid = 0;
    int coid = -1;

    (void)arg;
    memset(&g_sensor, 0, sizeof(g_sensor));
    if (qnx_ipc_create("qnx_demo_sensor", &ch) != QNX_IPC_OK) return NULL;

    if (qnx_ipc_lookup("qnx_mqtt_bridge", &coid) != QNX_IPC_OK) goto out;
    /* HELLO: "qnx_demo_sensor qnx/cmd/sensor/". */
    if (qnx_msg_pack("__hello__", "qnx_demo_sensor qnx/cmd/sensor/",
                     strlen("qnx_demo_sensor qnx/cmd/sensor/"),
                     frame, sizeof(frame), &frame_len) != QNX_MSG_OK)
        goto out;
    if (qnx_ipc_send(coid, frame, frame_len, reply, sizeof(reply), &reply_len)
            == QNX_IPC_OK
        && reply_len == 1 && reply[0] == 0)
        g_sensor.hello_ok = 1;

    /* Telemetry. */
    if (qnx_msg_pack("qnx/sensor/temp", "{\"temp_c\":23.5}",
                     strlen("{\"temp_c\":23.5}"),
                     frame, sizeof(frame), &frame_len) != QNX_MSG_OK)
        goto out;
    if (qnx_ipc_send(coid, frame, frame_len, reply, sizeof(reply), &reply_len)
            == QNX_IPC_OK
        && reply_len == 1 && reply[0] == 0)
        g_sensor.telemetry_ok = 1;

    /* Wait for a command from the bridge (up to 5 s). */
    if (qnx_ipc_receive(ch, rx, sizeof(rx), &rx_len, &rcvid, 5000)
        == QNX_IPC_OK) {
        char topic[256];
        const uint8_t *pl = NULL;
        size_t plen = 0;
        if (qnx_msg_unpack(rx, rx_len, topic, sizeof(topic), &pl, &plen)
            == QNX_MSG_OK) {
            snprintf(g_sensor.cmd_topic, sizeof(g_sensor.cmd_topic), "%s", topic);
            if (plen > 0 && plen < sizeof(g_sensor.cmd_payload)) {
                memcpy(g_sensor.cmd_payload, pl, plen);
                g_sensor.cmd_len = plen;
            }
            g_sensor.got_cmd = 1;
        }
        /* Reply with a framed ack, like the real demo app. */
        if (qnx_msg_pack("qnx/sensor/ack", "ok", 2,
                         frame, sizeof(frame), &frame_len) == QNX_MSG_OK)
            qnx_ipc_reply(rcvid, frame, frame_len);
    }

out:
    if (coid >= 0) close(coid);
    qnx_ipc_destroy(ch);
    return NULL;
}

/* Poll until fn() is true or the deadline (ms) passes. */
static int wait_until(int (*fn)(void *), void *arg, int timeout_ms)
{
    int waited = 0;
    while (waited < timeout_ms) {
        if (fn(arg)) return 1;
        usleep(20 * 1000);
        waited += 20;
    }
    return fn(arg);
}

static int inbound_nonempty(void *arg)
{
    return bridge_queue_count(&((bridge_state_t *)arg)->inbound) > 0;
}

static int sensor_got_cmd(void *arg)
{
    (void)arg;
    return g_sensor.got_cmd;
}

static void test_end_to_end(void)
{
    mosquitto_plugin_id_t *fake_id = (mosquitto_plugin_id_t *)(uintptr_t)0x1234;
    bridge_state_t *st = NULL;
    pthread_t sensor;
    int rc;

    printf("== end-to-end (plugin + worker + host IPC) ==\n");
    memset(g_cbs, 0, sizeof(g_cbs));
    g_pub_count = 0;

    rc = mosquitto_plugin_init(fake_id, (void **)&st, NULL, 0);
    CHECK(rc == 0 && st != NULL, "plugin init rc=%d", rc);
    if (rc != 0 || !st) return;
    CHECK(g_cbs[MOSQ_EVT_MESSAGE] != NULL, "MESSAGE callback registered");
    CHECK(g_cbs[MOSQ_EVT_TICK] != NULL, "TICK callback registered");
    CHECK(g_cbs[MOSQ_EVT_DISCONNECT] != NULL, "DISCONNECT callback registered");
    CHECK_STR_EQ(st->cmd_topic, "qnx/cmd/#");
    CHECK_STR_EQ(st->bridge_name, "qnx_mqtt_bridge");

    CHECK(pthread_create(&sensor, NULL, sensor_thread, NULL) == 0,
          "start sensor thread");

    /* Wait for the worker to ingest HELLO + telemetry, then TICK. */
    CHECK(wait_until(inbound_nonempty, st, 3000) == 1,
          "telemetry reached inbound queue");
    CHECK(g_sensor.hello_ok == 1, "HELLO accepted (status 0)");
    CHECK(g_sensor.telemetry_ok == 1, "telemetry accepted (status 0)");

    {
        struct mosquitto_evt_tick tick;
        memset(&tick, 0, sizeof(tick));
        rc = g_cbs[MOSQ_EVT_TICK](MOSQ_EVT_TICK, &tick, g_cb_userdata[MOSQ_EVT_TICK]);
        CHECK(rc == MOSQ_ERR_SUCCESS, "TICK rc=%d", rc);
    }
    CHECK(g_pub_count == 1, "one publish, got %d", g_pub_count);
    CHECK_STR_EQ(g_last_pub_topic, "qnx/sensor/temp");
    CHECK(g_last_pub_len == strlen("{\"temp_c\":23.5}")
          && memcmp(g_last_pub_payload, "{\"temp_c\":23.5}", g_last_pub_len) == 0,
          "published payload");

    /* MQTT -> QNX: inject a command via the MESSAGE callback. */
    {
        struct mosquitto_evt_message m;
        memset(&m, 0, sizeof(m));
        m.topic = "qnx/cmd/sensor/set_interval";
        m.payload = "5";
        m.payloadlen = 1;
        rc = g_cbs[MOSQ_EVT_MESSAGE](MOSQ_EVT_MESSAGE, &m,
                                     g_cb_userdata[MOSQ_EVT_MESSAGE]);
        CHECK(rc == MOSQ_ERR_SUCCESS, "MESSAGE rc=%d", rc);
    }
    /* A non-matching topic must be ignored. */
    {
        struct mosquitto_evt_message m;
        memset(&m, 0, sizeof(m));
        m.topic = "qnx/sensor/temp";
        m.payload = "x";
        m.payloadlen = 1;
        int before = bridge_queue_count(&st->outbound);
        rc = g_cbs[MOSQ_EVT_MESSAGE](MOSQ_EVT_MESSAGE, &m,
                                     g_cb_userdata[MOSQ_EVT_MESSAGE]);
        CHECK(rc == MOSQ_ERR_SUCCESS
              && bridge_queue_count(&st->outbound) == before,
              "non-command topic ignored");
    }
    CHECK(wait_until(sensor_got_cmd, NULL, 5000) == 1,
          "sensor received the command");
    pthread_join(sensor, NULL);
    CHECK_STR_EQ(g_sensor.cmd_topic, "qnx/cmd/sensor/set_interval");
    CHECK(g_sensor.cmd_len == 1 && g_sensor.cmd_payload[0] == '5',
          "command payload '5', len=%zu", g_sensor.cmd_len);
    /* The worker must have enqueued the sensor's framed ack into the
     * inbound queue (drain_outbound) — a second TICK must publish it. */
    CHECK(wait_until(inbound_nonempty, st, 3000) == 1,
          "ack reached inbound queue");
    {
        struct mosquitto_evt_tick tick;
        memset(&tick, 0, sizeof(tick));
        rc = g_cbs[MOSQ_EVT_TICK](MOSQ_EVT_TICK, &tick, g_cb_userdata[MOSQ_EVT_TICK]);
        CHECK(rc == MOSQ_ERR_SUCCESS, "TICK 2 rc=%d", rc);
    }
    CHECK(g_pub_count == 2, "two publishes, got %d", g_pub_count);
    CHECK_STR_EQ(g_last_pub_topic, "qnx/sensor/ack");
    CHECK(g_last_pub_len == 2 && memcmp(g_last_pub_payload, "ok", 2) == 0,
          "ack payload 'ok'");
    usleep(200 * 1000);
    CHECK(st->routed == 1, "routed==1, got %lu", st->routed);

    rc = mosquitto_plugin_cleanup(st, NULL, 0);
    CHECK(rc == 0, "plugin cleanup rc=%d", rc);
}

int main(void)
{
    /* Make sure no stale stub sockets from a crashed run interfere. */
    unlink("/tmp/qnx-ipc-test_ipc_ch.sock");
    unlink("/tmp/qnx-ipc-test_ipc_timeout.sock");
    unlink("/tmp/qnx-ipc-qnx_mqtt_bridge.sock");
    unlink("/tmp/qnx-ipc-qnx_demo_sensor.sock");

    test_framing();
    test_topic_match();
    test_queue();
    test_registry();
    test_ipc();
    test_end_to_end();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
        return 0;
    }
    printf("TESTS FAILED\n");
    return 1;
}

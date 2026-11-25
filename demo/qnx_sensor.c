/*
 * qnx_sensor.c — native QNX demo app for the qnx-mqtt-gateway.
 *
 * A simulated temperature sensor. It attaches the channel
 * "qnx_demo_sensor", registers itself with the bridge (HELLO), then
 * sends telemetry to the bridge every N seconds and answers commands
 * that arrive from MQTT through the bridge.
 *
 *   telemetry : topic qnx/sensor/temp
 *               payload {"temp_c":23.5,"unit":"C"}
 *               slow sine drift, 23.0..26.0 C, 60 s period, deterministic
 *   commands  : topic qnx/cmd/sensor/<cmd>, payload = args
 *               set_interval <1-60>  — telemetry period in seconds
 *               led <on|off>         — simulated LED state
 *               ping                 — reply "pong" on qnx/sensor/status
 *
 * Every received command and every sent telemetry line is printed to
 * stdout — that output IS the demo evidence on the QEMU console.
 *
 * Threading (why two threads): the bridge worker thread MsgSends
 * commands to us and waits for our reply, while we MsgSend telemetry
 * to the bridge and wait for its reply. If one thread did both, a
 * telemetry send issued while the worker is mid-command-send would
 * deadlock: each side blocked sending to the other, neither receiving.
 * So the main thread ONLY receives (and replies), and a dedicated
 * sender thread ONLY sends telemetry. The main thread is therefore
 * always in MsgReceive and the worker's command send can never block
 * on us. By the same rule the ping "pong" is sent back inside the
 * command reply itself (topic qnx/sensor/status), never as a second
 * nested MsgSend to the bridge.
 *
 * IPC API: plugin/qnx_ipc.h, framing: common/qnx_msg.h
 * (see DESIGN.md — the contract this file is written against).
 */

#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "qnx_ipc.h"
#include "qnx_msg.h"

#define DEMO_CHANNEL   "qnx_demo_sensor"
#define BRIDGE_CHANNEL "qnx_mqtt_bridge"

#define CMD_PREFIX   "qnx/cmd/sensor/"
#define TOPIC_TEMP   "qnx/sensor/temp"
#define TOPIC_STATUS "qnx/sensor/status"
#define TOPIC_ACK    "qnx/sensor/ack"

#define MAX_MSG        4096
#define HELLO_RETRIES  60       /* 60 x 1 s waiting for the bridge */
#define RECV_TIMEOUT_MS 1000    /* main-thread receive poll slice */

static volatile sig_atomic_t g_stop = 0;
static volatile int g_interval_s = 2;   /* telemetry period, 1..60 */
static volatile int g_led_on = 0;       /* simulated LED */
static time_t g_start;                  /* for the deterministic temp drift */
static int g_bridge_coid = -1;          /* owned by the sender thread */

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* usleep() is gone from POSIX.1-2008; nanosleep is the portable way. */
static void sleep_ms(long ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000 * 1000;
    nanosleep(&ts, NULL);
}

/* Sine without libm: this SDP install ships no math.h and no libm, so
 * use a small Taylor approximation (error ~1e-7 after range reduction
 * to [-pi, pi]). Deterministic, same output shape as sin(). */
static double demo_sin(double x)
{
    const double pi = 3.14159265358979323846;
    const double two_pi = 6.28318530717958647692;
    double x2;
    while (x > pi) x -= two_pi;
    while (x < -pi) x += two_pi;
    x2 = x * x;
    return x * (1.0 - x2 * (1.0 / 6.0 - x2 * (1.0 / 120.0 -
           x2 * (1.0 / 5040.0 - x2 * (1.0 / 362880.0)))));
}

/* Temperature: sine drift, 23.0..26.0 C, 60 s period, deterministic. */
static double current_temp_c(void)
{
    double t = difftime(time(NULL), g_start);
    return 24.5 + 1.5 * demo_sin(2.0 * 3.14159265358979323846 * t / 60.0);
}

/* MsgSend one framed message to the bridge; returns 0 on ok. */
static int send_to_bridge(int bridge_coid, const char *topic,
                          const char *payload)
{
    uint8_t buf[MAX_MSG];
    size_t len = 0;
    uint8_t status = 0;
    size_t reply_len = 0;

    if (qnx_msg_pack(topic, payload, strlen(payload),
                     buf, sizeof(buf), &len) != 0) {
        fprintf(stderr, "[sensor] pack failed for topic %s\n", topic);
        return -1;
    }
    if (qnx_ipc_send(bridge_coid, buf, len,
                     &status, sizeof(status), &reply_len) != 0) {
        fprintf(stderr, "[sensor] send to bridge failed (topic %s)\n", topic);
        return -1;
    }
    /* Success is status == 0 ONLY. QNX MsgSend never reports the reply
     * length, and qnx_ipc_send only fills *reply_len for framed replies
     * (>= 12 bytes with a valid header) — the bridge's 1-byte status
     * reply always reports reply_len == 0 on QNX. (The host stub
     * reports it correctly, which is why the host tests never caught
     * this.) Requiring reply_len >= 1 made every QNX send look like
     * a NAK even though the bridge accepted the message. */
    (void)reply_len;
    if (status != 0) {
        fprintf(stderr, "[sensor] bridge NAK for topic %s (status %u)\n",
                topic, (unsigned)status);
        return -1;
    }
    return 0;
}

/*
 * Telemetry sender thread: every g_interval_s seconds, send one
 * temperature reading to the bridge. Sleeps in 100 ms slices so an
 * interval change or a shutdown takes effect promptly. On a send
 * failure (bridge restarted?) it re-looks the bridge up once and
 * retries on the next tick instead of dying.
 */
static void *telemetry_thread(void *arg)
{
    (void)arg;

    for (;;) {
        /* sleep one interval, in slices, watching g_stop / g_interval_s */
        int waited_ms = 0;
        int target_ms = g_interval_s * 1000;
        while (waited_ms < target_ms && !g_stop) {
            sleep_ms(100);
            waited_ms += 100;
            if (g_interval_s * 1000 != target_ms)
                target_ms = g_interval_s * 1000;  /* interval changed */
        }
        if (g_stop)
            break;

        char payload[64];
        snprintf(payload, sizeof(payload),
                 "{\"temp_c\":%.1f,\"unit\":\"C\"}", current_temp_c());

        if (send_to_bridge(g_bridge_coid, TOPIC_TEMP, payload) == 0) {
            printf("[sensor] telemetry %s %s\n", TOPIC_TEMP, payload);
            fflush(stdout);
        } else if (qnx_ipc_lookup(BRIDGE_CHANNEL, &g_bridge_coid) == 0) {
            printf("[sensor] bridge re-looked-up, will retry telemetry\n");
            fflush(stdout);
        }
    }
    return NULL;
}

/*
 * Handle one command frame. Fills ack_out with the human-readable ack
 * text and returns the topic the reply must carry: TOPIC_STATUS for
 * ping (the "pong" IS the reply — see the header comment), TOPIC_ACK
 * for everything else.
 */
static const char *handle_command(const char *topic,
                                  const uint8_t *payload, size_t payload_len,
                                  char *ack_out, size_t ack_cap)
{
    const char *cmd = topic + strlen(CMD_PREFIX);
    char args[256];
    size_t n = payload_len < sizeof(args) - 1 ? payload_len : sizeof(args) - 1;

    memcpy(args, payload, n);
    args[n] = '\0';

    printf("[sensor] command %s args=\"%s\"\n", topic, args);

    if (strcmp(cmd, "set_interval") == 0) {
        int secs = atoi(args);
        if (secs < 1)
            secs = 1;
        if (secs > 60)
            secs = 60;
        g_interval_s = secs;
        printf("[sensor] telemetry interval set to %d s\n", g_interval_s);
        snprintf(ack_out, ack_cap, "set_interval %d ok", g_interval_s);
    } else if (strcmp(cmd, "led") == 0) {
        if (strcmp(args, "on") == 0) {
            g_led_on = 1;
        } else if (strcmp(args, "off") == 0) {
            g_led_on = 0;
        } else {
            printf("[sensor] LED bad arg: \"%s\" (want on|off)\n", args);
            snprintf(ack_out, ack_cap,
                     "led: bad arg \"%s\", want on|off", args);
            fflush(stdout);
            return TOPIC_ACK;
        }
        printf("[sensor] LED %s\n", g_led_on ? "ON" : "OFF");
        snprintf(ack_out, ack_cap, "led %s ok", g_led_on ? "on" : "off");
    } else if (strcmp(cmd, "ping") == 0) {
        printf("[sensor] ping -> pong\n");
        snprintf(ack_out, ack_cap,
                 "{\"status\":\"pong\",\"led\":\"%s\"}",
                 g_led_on ? "on" : "off");
        fflush(stdout);
        return TOPIC_STATUS;
    } else {
        printf("[sensor] unknown command: %s\n", cmd);
        snprintf(ack_out, ack_cap, "unknown command: %s", cmd);
    }
    fflush(stdout);
    return TOPIC_ACK;
}

int main(void)
{
    qnx_ipc_channel_t *ch = NULL;
    pthread_t sender;
    int rc;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    g_start = time(NULL);

    if (qnx_ipc_create(DEMO_CHANNEL, &ch) != 0) {
        fprintf(stderr, "[sensor] cannot attach channel %s\n", DEMO_CHANNEL);
        return 1;
    }
    printf("[sensor] channel %s attached\n", DEMO_CHANNEL);
    fflush(stdout);

    /* Find the bridge, retrying while it starts up. */
    for (int i = 0; i < HELLO_RETRIES && !g_stop; i++) {
        if (qnx_ipc_lookup(BRIDGE_CHANNEL, &g_bridge_coid) == 0)
            break;
        if (i == 0)
            printf("[sensor] waiting for bridge %s ...\n", BRIDGE_CHANNEL);
        sleep(1);
    }
    if (g_bridge_coid < 0) {
        fprintf(stderr, "[sensor] bridge %s never appeared, giving up\n",
                BRIDGE_CHANNEL);
        qnx_ipc_destroy(ch);
        return 1;
    }

    /* Register with the bridge: HELLO, payload = "<channel> <prefix>".
     * The prefix MUST be explicit: without it the bridge derives
     * "qnx/cmd/demo/sensor/" from the channel name, but the demo's
     * command topics live under "qnx/cmd/sensor/". */
    {
        char hello_payload[128];
        snprintf(hello_payload, sizeof(hello_payload), "%s %s",
                 DEMO_CHANNEL, CMD_PREFIX);
        if (send_to_bridge(g_bridge_coid, "__hello__", hello_payload) != 0) {
            fprintf(stderr, "[sensor] HELLO to bridge failed\n");
            qnx_ipc_destroy(ch);
            return 1;
        }
    }
    printf("[sensor] HELLO sent to bridge (registered as %s)\n", DEMO_CHANNEL);
    printf("[sensor] telemetry every %d s on %s\n", g_interval_s, TOPIC_TEMP);
    fflush(stdout);

    if (pthread_create(&sender, NULL, telemetry_thread, NULL) != 0) {
        fprintf(stderr, "[sensor] cannot start telemetry thread\n");
        qnx_ipc_destroy(ch);
        return 1;
    }

    /* Main thread: receive commands, reply to each one. */
    uint8_t buf[MAX_MSG];
    while (!g_stop) {
        size_t len = 0;
        int rcvid = 0;

        rc = qnx_ipc_receive(ch, buf, sizeof(buf), &len, &rcvid,
                             RECV_TIMEOUT_MS);
        if (rc == 1)          /* timeout: nothing to do, loop again */
            continue;
        if (rc == 2)          /* pulse: ignore (see DESIGN.md) */
            continue;
        if (rc != 0) {
            fprintf(stderr, "[sensor] receive error %d\n", rc);
            sleep_ms(100);
            continue;
        }

        /* A real message arrived: unpack, handle, always reply. */
        char topic[256];
        const uint8_t *payload = NULL;
        size_t payload_len = 0;
        char ack[512];
        const char *reply_topic = TOPIC_ACK;

        if (qnx_msg_unpack(buf, len, topic, sizeof(topic),
                           &payload, &payload_len) != 0) {
            fprintf(stderr, "[sensor] bad frame received, dropped\n");
            snprintf(ack, sizeof(ack), "error: bad frame");
        } else if (strncmp(topic, CMD_PREFIX, strlen(CMD_PREFIX)) == 0) {
            reply_topic = handle_command(topic, payload, payload_len,
                                         ack, sizeof(ack));
        } else {
            printf("[sensor] ignored message, topic %s is not a command\n",
                   topic);
            snprintf(ack, sizeof(ack), "ignored: not a sensor command");
            fflush(stdout);
        }

        uint8_t ack_buf[MAX_MSG];
        size_t ack_len = 0;
        if (qnx_msg_pack(reply_topic, ack, strlen(ack),
                         ack_buf, sizeof(ack_buf), &ack_len) == 0) {
            if (qnx_ipc_reply(rcvid, ack_buf, ack_len) != 0)
                fprintf(stderr, "[sensor] reply failed\n");
        } else {
            fprintf(stderr, "[sensor] ack pack failed\n");
        }
    }

    printf("[sensor] shutting down\n");
    pthread_join(sender, NULL);
    qnx_ipc_destroy(ch);
    return 0;
}

# qnx-mqtt-gateway — Design Contract (v1, 25 Nov 2026)

**Goal:** Mosquitto broker plugin (v5) bridging MQTT topics ↔ QNX native message
passing, plus a native QNX demo app. Built ON TOP of the official Mosquitto
2.0.15 QNX port (github.com/qnx-ports/mosquitto) — we do NOT re-port Mosquitto.

**Target categories:** Open-Source Ports + Best Use of QNX Features ($3.500).
**First target:** x86_64 QEMU (QSTI image). aarch64 = stretch goal only.

## API facts (verified 25 Nov 2026)
- `third-party/mosquitto/upstream-mosquitto_plugin.h` (420 lines) and
  `third-party/mosquitto/upstream-mosquitto_broker.h` (561 lines) are **byte-identical** to the
  qnx-ports fork (`qnx_v2.0.15` branch). Code against these headers.
- Broker is single-threaded `poll()` loop → **all blocking QNX IPC MUST run in the
  plugin's own worker thread**, never in a hook callback.

## Layout
```
gateway/
  DESIGN.md            (this file — the contract; do not change interfaces without updating it)
  README.md            (user-facing: what/why/how to build, run, test)
  LICENSE              (MIT for our code)
  plugin/
    qnx_bridge.c       — Mosquitto v5 plugin (hooks, queues, worker thread)
    qnx_bridge.h
    qnx_ipc.h          — IPC abstraction (same API, two backends)
    qnx_ipc_qnx.c      — QNX backend: name_attach/name_open, ChannelCreate,
                         MsgSend/MsgReceive/MsgReply, TimerTimeout wakeups
    qnx_ipc_host.c     — host stub backend: Unix-domain socket, same semantics,
                         ONLY for host-side logic tests (never shipped to target)
    Makefile.qnx       — qcc build, x86_64 (aarch64 variant commented)
    Makefile.host      — gcc build for host tests (headers: third-party/mosquitto/)
    CMakeLists.txt     — host build (optional convenience)
  demo/
    qnx_sensor.c       — native QNX demo app (simulated temperature sensor)
    Makefile.qnx
    Makefile.host
  test/
    test_gateway.c     — C unit test: framing, topic match, queue, registry
    run_tests.sh       — builds host binaries + runs unit test
  docs/
    TEST-PLAN.md       — QEMU demo procedure with expected console transcripts
  scripts/
    run_demo.sh        — host-side helper: start broker+plugin, pub/sub demo
```

## Wire protocol (QNX message passing)
One framed message, max 4096 bytes total:
```c
typedef struct {
    uint32_t magic;        /* 'QMBR' = 0x514D4252 */
    uint32_t topic_len;    /* bytes, no NUL */
    uint32_t payload_len;  /* bytes, no NUL */
    uint8_t  data[];       /* topic bytes, then payload bytes */
} qnx_mqtt_msg_t;
```
- `magic` validated on receive; bad magic → drop + log.
- `topic_len`/`payload_len` bounds-checked against the received byte count.

## IPC API (`plugin/qnx_ipc.h` — identical on QNX and host stub)
```c
typedef struct qnx_ipc_channel qnx_ipc_channel_t; /* opaque */
int  qnx_ipc_create(const char *name, qnx_ipc_channel_t **out);
void qnx_ipc_destroy(qnx_ipc_channel_t *ch);
int  qnx_ipc_lookup(const char *name, int *coid_out);
int  qnx_ipc_send(int coid, const void *buf, size_t len,
                  void *reply, size_t reply_cap, size_t *reply_len);
int  qnx_ipc_receive(qnx_ipc_channel_t *ch, void *buf, size_t cap,
                     size_t *len_out, int *rcvid_out, int timeout_ms);
int  qnx_ipc_reply(int rcvid, const void *buf, size_t len);
int  qnx_ipc_wake(qnx_ipc_channel_t *ch); /* pulse own channel: unblock receive */
/* Framing (portable; implemented in common/qnx_msg.c, header common/qnx_msg.h) */
int qnx_msg_pack(const char *topic, const void *payload, size_t payload_len,
                 uint8_t *out, size_t cap, size_t *out_len);
int qnx_msg_unpack(const uint8_t *buf, size_t len,
                   char *topic_out, size_t topic_cap,
                   const uint8_t **payload_out, size_t *payload_len_out);
```
Return conventions: 0 = ok; `qnx_ipc_receive` returns 1 on timeout, 2 on pulse
(caller ignores), <0 on error. All functions log to stderr on error when
`QNX_IPC_DEBUG` env (host) / plugin log level (target) is on. QNX backend is a
thin wrapper over ChannelCreate/name_attach/MsgSend/MsgReceive/MsgReply/
TimerTimeout; host stub emulates the same semantics over a Unix-domain socket.

## Channels (QNX name_attach / name_open)
- `qnx_mqtt_bridge` — owned by the plugin worker thread. QNX apps MsgSend
  telemetry here; bridge MsgReplies with a 1-byte status (0 = ok).
- `qnx_demo_sensor` — owned by the demo app. Plugin worker thread MsgSends
  commands here; demo MsgReplies with `qnx_mqtt_msg_t` ack (topic `qnx/sensor/ack`).
- Registration: on start, a QNX app sends a HELLO message to the bridge channel:
  topic `__hello__`, payload = its own channel name (e.g. `qnx_demo_sensor`).
  Bridge keeps a fixed registry (max 8 apps): channel name → coid + topic prefix.
  Commands from MQTT are routed by longest-prefix match on the registered
  topic prefixes. Unknown prefix → drop + log.

## Plugin behaviour
- Hooks registered in `mosquitto_plugin_init()`:
  - `MOSQ_EVT_MESSAGE` — if topic matches `qnx/cmd/#` (configurable, see below):
    copy topic+payload into the **outbound queue** (bounded 64 msgs; overflow =
    drop oldest + count). NEVER MsgSend from this hook.
  - `MOSQ_EVT_TICK` — drain the **inbound queue**; for each message call
    `mosquitto_broker_publish()` (retain=0, qos=0). Log publish failures.
  - `MOSQ_EVT_DISCONNECT` / `MOSQ_EVT_BASIC_AUTH` — not used in v1 (log only).
- Worker thread (created in init, joined in cleanup):
  - Loop: `TimerTimeout(100ms)` then `MsgReceive()` on the bridge channel.
    On timeout (rcvid==0 / _PULSE_CODE... use `MsgReceive` returning 0 with
    TimerTimeout armed) → drain outbound queue via `MsgSend()` to the
    registered app channel (per routing), expecting a reply within the
    MsgSend (demo replies immediately; if MsgSend fails, log + drop).
  - On `MsgReceive` with a real message → validate framing → push to inbound
    queue → `MsgReply(rcvid, 0-status-byte)`.
  - On `MsgReceive` pulse (rcvid==0 with pulse code) → ignore (document why).
  - Shutdown: set `atomic_bool stop`, `MsgSendPulse` to own channel to wake
    the thread, `pthread_join`.
- Thread-safety: both queues protected by one `pthread_mutex_t` each + a
  `pthread_cond_t` is NOT needed (TICK polls; worker polls on 100 ms timeout).
  Keep it simple and obviously correct; document the locking.
- Config (mosquitto.conf `plugin_opt_*`):
  - `plugin_opt_qnx_cmd_topic`   default `qnx/cmd/#` (MQTT→QNX filter)
  - `plugin_opt_qnx_bridge_name` default `qnx_mqtt_bridge`
  - `plugin_opt_qnx_log`         default `1` (0 = quiet)

## Demo app (`qnx_sensor.c`)
- `name_attach("qnx_demo_sensor")`, HELLO to bridge on start.
- Every 2 s (configurable via command): telemetry MsgSend to bridge,
  topic `qnx/sensor/temp`, payload JSON `{"temp_c":23.5,"unit":"C"}` with a
  slow sine drift (23.0–26.0 °C) — deterministic, no rand() seeding surprises.
- Commands (topic `qnx/cmd/sensor/<cmd>`, payload = args):
  - `set_interval <secs>` — change telemetry period (1–60, clamp).
  - `led <on|off>` — simulated LED state, printed + reported.
  - `ping` — reply `pong` as telemetry topic `qnx/sensor/status`.
- Prints every received command and every sent telemetry line to stdout
  (this IS the demo evidence in the QEMU console).

## Build
- QNX (`Makefile.qnx`): `qcc -Vgcc_ntox86_64 -shared -fPIC` for the plugin
  (`-I$QNX_TARGET/usr/local/include -L$QNX_TARGET/usr/local/lib -lmosquitto`
  — headers/libs come from the official Mosquitto port install);
  demo app: `qcc -Vgcc_ntox86_64 -lsocket`. Guard with a clear error if
  `QNX_TARGET` is unset. aarch64: same file with `gcc_ntoaarch64le` (commented
  block, untested until toolchain lands).
- Host (`Makefile.host` / `CMakeLists.txt`): plain `gcc`, include path
  `../third-party/mosquitto` (the verified headers), link `qnx_ipc_host.c`. The host
  build MUST succeed and `test/run_tests.sh` MUST pass before the code is
  called done — this is the only compile proof available until qcc lands.
- **Do NOT claim a QNX compile/test success without the toolchain.** Any
  QNX-side claim must cite a real log.

## Docs to produce
- `README.md`: what it is (and what it is NOT — no Mosquitto re-port), arch
  diagram (ASCII), build steps (host test + QNX cross build), run steps
  (broker config snippet with `plugin` + `plugin_opt_*` lines), demo transcript.
- `docs/TEST-PLAN.md`: exact QEMU boot (QSTI `mkqnximage --run`), file copy
  (`scp` plugin/demo/mosquitto.conf), broker start, host-side
  `mosquitto_pub/sub` commands, expected console output line-by-line, and
  what counts as PASS/FAIL per step.

## Non-goals (v1)
- No TLS, no auth/ACL plugin, no MQTT v5 properties, no retained messages.
- No aarch64 proof, no CTI image, no Devpost page, no video (later milestones).

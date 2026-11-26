# qnx-mqtt-gateway

A Mosquitto broker plugin (v5) that bridges MQTT topics to QNX native
message passing, and back. A native QNX app can publish telemetry to
MQTT by `MsgSend()`-ing to the bridge channel, and any MQTT client can
send it commands by publishing to a `qnx/cmd/#` topic.

Entry for the QNX Global Hackathon, in the categories Open-Source
Ports and Best Use of QNX Features.

## What this is, and what it is not

This is a small plugin plus a demo app, built on top of the official
Mosquitto 2.0.15 QNX port (`github.com/qnx-ports/mosquitto`).

It does not re-port Mosquitto. The broker, the client library, and
the dependencies were already ported, and that port already works.
Re-doing it would add nothing. The work here is the bridge between
that broker and QNX native IPC, the part the port does not do.

## Status

The gateway runs end to end on QEMU x86_64. The last full run
(v10, 25 Nov 2026) stayed up for 600 seconds and produced:

- 115 telemetry messages from the sensor (`qnx/sensor/temp`, one
  every 2 seconds, then every 5 seconds after the interval command)
- all three demo commands received: `set_interval`, `led`, `ping`
- both command acks published (`qnx/sensor/ack set_interval 5 ok`,
  `qnx/sensor/ack led on ok`)
- the ping answered on `qnx/sensor/status` with payload
  `{"status":"pong","led":"on"}`
- 0 NAK replies and 0 dropped outbound messages

The host unit tests pass too: 241 checks, 0 failures
(`test/run_tests.sh`).

Verified on SDP 8.0 and SDP 8.0.6 with zero code changes: the
full 600-second run was repeated on a clean 8.0.6 tree on
27 Nov 2026 with identical results (115 telemetry, all three
commands, pong, 0 NAK, 0 drops).

## Architecture

```
  MQTT client (mosquitto_pub / your app)
        |  publish qnx/cmd/sensor/led on
        v
  +---------------- mosquitto broker (official QNX port 2.0.15) -----+
  |  plugin hook MOSQ_EVT_MESSAGE  (qnx/cmd/# only)                  |
  |     -> copies topic+payload into the bounded outbound queue       |
  |  plugin hook MOSQ_EVT_TICK                                        |
  |     -> drains inbound queue via mosquitto_broker_publish()        |
  |  plugin worker thread (blocking IPC lives ONLY here; the broker   |
  |  itself is a single-threaded poll() loop and must never block)    |
  |     -> MsgReceive() on qnx_mqtt_bridge, MsgSend() commands out    |
  +-------------------------------------------------------------------+
        | MsgSend / MsgReply (QNX native message passing)
        v
  +---------------- qnx_sensor (native QNX demo app) -----------------+
  |  channel qnx_demo_sensor                                          |
  |  HELLO on start -> registers with the bridge                      |
  |  telemetry every 2 s: qnx/sensor/temp                             |
  |     {"temp_c":23.5,"unit":"C"}  (sine drift 23.0..26.0 C)          |
  |  commands: set_interval 1-60 | led on|off | ping -> pong           |
  +-------------------------------------------------------------------+
```

Wire format between bridge and apps: one framed message, max 4096 bytes
(`common/qnx_msg.h`):

```
magic 'QMBR' | topic_len | payload_len | topic bytes | payload bytes
```

Channels: `qnx_mqtt_bridge` (owned by the plugin worker thread),
`qnx_demo_sensor` (owned by the demo app). Registration is a HELLO
message, topic `__hello__`, payload = the app's own channel name. The
bridge keeps a fixed registry (max 8 apps) and routes commands by
longest-prefix match. Full contract: `DESIGN.md`.

## Layout

```
  DESIGN.md            the contract (interfaces, protocol, behaviour)
  README.md            this file
  LICENSE              MIT for our code
  common/              framed message pack/unpack (portable C)
  plugin/              bridge plugin + IPC abstraction (QNX + host stub)
  demo/                qnx_sensor demo app + Makefiles
  test/                host unit tests + QNX-side helper programs
  docs/TEST-PLAN.md    step-by-step QEMU demo procedure
  scripts/run_demo.sh  host-side demo helper
  ifs/                 minimal IFS image: minimal.build + mosquitto.conf
  third-party/         Mosquitto 2.0.15 headers, vendored (see below)
```

The Mosquitto headers in `third-party/mosquitto/` are copied verbatim
from upstream Eclipse Mosquitto 2.0.15. They are byte-identical to the
headers in the official QNX port, and they are only needed so the
plugin compiles without a Mosquitto install. On a QNX target with the
official port installed, the port provides the same headers under
`$QNX_TARGET/usr/local/include`. See
`third-party/mosquitto/README.md`.

## Prerequisites

- A Linux host with `gcc` and `make` (for the host build and tests)
- A myQNX account and QNX SDP 8.0 Non-Commercial (for the QNX cross
  build, the IFS image, and QEMU). Set `QNX_HOST` and `QNX_TARGET`
  by hand; this SDP install has no `qnxsdp-env.sh`:

```sh
export QNX_HOST=$HOME/qnx/qnx800/host/linux/x86_64
export QNX_TARGET=$HOME/qnx/qnx800/target/qnx
export PATH=$QNX_HOST/usr/bin:$PATH
qcc -V               # must list gcc_ntox86_64
```

- QEMU (8.2.2 was used for the verified run), with at least 1 GB of
  RAM free for the guest
- `mosquitto_pub` / `mosquitto_sub` (Mosquitto clients, any 2.x) and
  `ssh` / `scp` for the demo steps in `docs/TEST-PLAN.md`

## Build

### Host test build (no QNX toolchain needed)

```sh
cd plugin && make -f Makefile.host
cd ../test && ./run_tests.sh        # 241 checks, 0 failures
cd ../demo  && make -f Makefile.host   # builds ./qnx_sensor_host
```

The host build uses the stub IPC backend (`plugin/qnx_ipc_host.c`, a
Unix-domain socket with the same semantics) so the logic can be tested
on Linux. It is never shipped to the target. Running the host build
also prepares `plugin/build/host-include/`, the symlinked Mosquitto
headers the QNX Makefile reuses (see below).

### QNX cross build (needs the SDP 8.0 toolchain)

```sh
export QNX_HOST=$HOME/qnx/qnx800/host/linux/x86_64
export QNX_TARGET=$HOME/qnx/qnx800/target/qnx
export PATH=$QNX_HOST/usr/bin:$PATH
cd plugin && make -f Makefile.qnx
cd ../demo    && make -f Makefile.qnx
```

Plugin: `qcc -Vgcc_ntox86_64 -shared -fPIC`, headers from the
vendored copies in `third-party/mosquitto/` (via
`plugin/build/host-include/`, prepared by the host build above).
Once the official Mosquitto port is installed into the SDP, switch
the include path to `$QNX_TARGET/usr/local/include`. Demo:
`qcc -Vgcc_ntox86_64`. Both Makefiles fail with a clear error
if `QNX_TARGET` is unset. aarch64 is a stretch goal (commented variant
in the Makefiles, untested). Build notes and the fixes from the first
qcc run live in `plugin/BUILD-QNX.md`.

Check the plugin exports before going further:

```sh
ntox86_64-nm -D plugin/qnx_bridge.so | grep mosquitto_plugin_
# must list: mosquitto_plugin_version, mosquitto_plugin_init,
#            mosquitto_plugin_cleanup
```

## Build the IFS image and boot it in QEMU

`ifs/minimal.build` is the mkifs script for a minimal x86_64 image:
procnto, slogger2, `random` (qcrypto), io-sock, the Mosquitto broker
from the official port, the bridge plugin, the demo sensor, and the
small helper programs in `test/` (`mark`, `mount_shm`,
`test_timer_timeout`). It expects the SDP 8.0 target tree plus the
built plugin and demo; edit the `[search=...]` paths at the top of
the file to match your install before building.

```sh
mkifs ifs/minimal.build minimal-ifs.bin
```

Boot the image under QEMU x86_64 with 1 GB of guest RAM. The image
runs its startup script on its own: it probes `/dev/shmem` (built
in to procnto, no separate mount needed), starts
slogger2, `random`, and io-sock, then the broker with
`ifs/mosquitto.conf`, then `qnx_sensor`, then subscribes to
`qnx/sensor/#` and sends the three demo commands itself. The full
step-by-step procedure, including the manual variant (copy files to
a running target with `scp`, start the broker and the demo by hand),
is in `docs/TEST-PLAN.md`.

## Run (on the QNX target)

`/etc/mosquitto/mosquitto.conf` (or wherever you keep it):

```conf
listener 1883
allow_anonymous true

plugin /usr/local/lib/mosquitto_qnx_bridge.so
plugin_opt_qnx_cmd_topic qnx/cmd/#
plugin_opt_qnx_bridge_name qnx_mqtt_bridge
plugin_opt_qnx_log 1
```

Start the broker, then the demo (order does not matter, the demo waits
up to 60 s for the bridge):

```sh
mosquitto -c /etc/mosquitto/mosquitto.conf -v &
/usr/local/bin/qnx_sensor &
```

From any machine that can reach the broker:

```sh
# watch the telemetry
mosquitto_sub -h <target-ip> -t 'qnx/sensor/#' -v

# send commands
mosquitto_pub -h <target-ip> -t qnx/cmd/sensor/set_interval -m 5
mosquitto_pub -h <target-ip> -t qnx/cmd/sensor/led -m on
mosquitto_pub -h <target-ip> -t qnx/cmd/sensor/ping -m ''
```

Or use the helper (checks prerequisites first, fails with a clear
message if anything is missing):

```sh
sh scripts/run_demo.sh <target-ip>
```

## How to verify a run

A good run shows all of this. If any line is missing, the run is
not a pass.

On the target console (`qnx_sensor` stdout):

```
[sensor] channel qnx_demo_sensor attached
[sensor] HELLO sent to bridge (registered as qnx_demo_sensor)
[sensor] telemetry every 2 s on qnx/sensor/temp
[sensor] telemetry qnx/sensor/temp {"temp_c":24.5,"unit":"C"}
[sensor] command qnx/cmd/sensor/set_interval args="5"
[sensor] telemetry interval set to 5 s
[sensor] command qnx/cmd/sensor/led args="on"
[sensor] LED ON
[sensor] command qnx/cmd/sensor/ping args=""
[sensor] ping -> pong
```

On the host subscriber (`mosquitto_sub -t 'qnx/sensor/#'`):

```
qnx/sensor/temp {"temp_c":24.5,"unit":"C"}
qnx/sensor/temp {"temp_c":24.7,"unit":"C"}
qnx/sensor/ack set_interval 5 ok
qnx/sensor/ack led on ok
qnx/sensor/status {"status":"pong","led":"on"}
```

And in the broker log: `mosquitto version 2.0.15 running`, the
plugin loaded, and no NAK and no dropped outbound messages. Exact
temperatures vary, the sine drift moves 23.0-26.0 C over 60 s.
After `set_interval 5`, the `qnx/sensor/temp` lines slow to one
every 5 s.

## Design notes worth knowing

- The broker never blocks. All `MsgSend`/`MsgReceive` runs in the
  plugin's worker thread; hooks only touch bounded queues.
- No cross-send deadlock. The demo's receive thread only receives
  (and replies); a separate sender thread only sends telemetry. The
  ping `pong` travels inside the command reply itself, never as a
  second nested `MsgSend` while the bridge is waiting for the reply.
- Bounded queues, drop-oldest. The outbound queue holds 64
  messages; overflow drops the oldest and counts it. A slow QNX app
  can never wedge the broker.
- v1 non-goals: no TLS, no auth/ACL plugin, no MQTT v5 properties,
  no retained messages, no aarch64 proof yet.

## Testing

- `test/run_tests.sh` builds the plugin and unit tests with host gcc
  and runs them: framing, topic match, queue, registry, host IPC,
  and an end-to-end plugin run. 241 checks, 0 failures. It must
  pass before any change is called done.
- `docs/TEST-PLAN.md` is the full QEMU demo procedure: boot, file
  copy, broker start, pub/sub commands, expected output
  line-by-line, and PASS/FAIL criteria per step.
- `test/test_timer_timeout.c` is a small reproducer for a
  `TimerTimeout` EINVAL the worker thread hit in early QEMU runs;
  it documents what was ruled out and what is still open. The
  gateway itself works around it; see the comments in the file.

## License

MIT, see `LICENSE`. The Mosquitto broker and its official QNX port
keep their own licences (EPL / BSD-style); this repo only licenses
the bridge plugin, the demo app, the helper programs, and the docs.
The vendored Mosquitto headers keep their upstream EPL-2.0 OR
BSD-3-Clause header.

# TEST-PLAN — QEMU demo procedure (QNX SDP 8.0, x86_64 QSTI)

> **Update, 25 Nov 2026:** the toolchain has since landed. The QNX
> cross build passed and the QEMU v10 run passed in full, see the
> Status section in `README.md`. The `[PENDING TOOLCHAIN]` marks
> below are kept as written; treat those steps as done.

How to take the gateway from a cold checkout to a working demo on the
QEMU target, with the exact console output to expect at each step and
what counts as PASS or FAIL.

**Honesty note:** steps marked **[PENDING TOOLCHAIN]** cannot be
executed yet — the SDP 8.0 toolchain and the QEMU image are still
being installed. They are written out in full now so the run is
mechanical once the toolchain lands; nothing below is claimed as
already run.

## Prerequisites (host)

- Linux host with `qcc` on PATH (after sourcing `qnxsdp-env.sh`)
- `mosquitto_pub` / `mosquitto_sub` (Mosquitto clients, any 2.x)
- `ssh` / `scp` to the target
- The QEMU Quick Start image (QSTI) for SDP 8.0

Check them:

```sh
which qcc mosquitto_pub mosquitto_sub ssh scp
echo $QNX_TARGET          # must be non-empty
```

## Step 0 — Build everything [PENDING TOOLCHAIN]

```sh
. ~/qnx/qnx800/qnxsdp-env.sh
cd gateway/plugin && make -f Makefile.qnx
cd ../demo    && make -f Makefile.qnx
cd ../test    && ./run_tests.sh
ls -l plugin/mosquitto_qnx_bridge.so demo/qnx_sensor
```

- **PASS:** both binaries exist, host unit tests print all PASS.
- **FAIL:** any compile error, or any unit test failure — stop, fix,
  do not proceed to the target. (Host unit tests can run today; the
  qcc builds cannot, until the toolchain lands.)

## Step 1 — Boot the QEMU target [PENDING TOOLCHAIN]

```sh
mkqnximage --run
```

Wait for the QNX login prompt on the QEMU console. Log in as root
(or the user the image provides). Find the target IP:

```sh
# on the target console
ifconfig
```

Note the address — call it `<target-ip>` below (typically a
192.168.x.x on the QEMU user-network TAP/bridge).

- **PASS:** login prompt appears, `ifconfig` shows an address.
- **FAIL:** image fails to boot, or no network address — check the
  QSTI image and the host network setup before continuing.

## Step 2 — Copy files to the target [PENDING TOOLCHAIN]

From the host (adjust paths to where you built):

```sh
scp gateway/plugin/mosquitto_qnx_bridge.so root@<target-ip>:/usr/local/lib/
scp gateway/demo/qnx_sensor          root@<target-ip>:/usr/local/bin/
scp gateway/mosquitto.conf           root@<target-ip>:/etc/mosquitto/mosquitto.conf
```

(`mosquitto.conf` is the snippet from README.md: `listener 1883`,
`allow_anonymous true`, the `plugin` line and the three
`plugin_opt_qnx_*` lines.)

- **PASS:** all three copies succeed; `ls -l` on the target shows the
  files.
- **FAIL:** scp refused / no route — verify `<target-ip>` and that
  sshd is running on the target.

## Step 3 — Start the broker [PENDING TOOLCHAIN]

On the target console:

```sh
mosquitto -c /etc/mosquitto/mosquitto.conf -v
```

Expected (broker log lines may vary in wording, but these must appear):

```
... mosquitto version 2.0.15 starting
... Opening ipv4 listen socket on port 1883.
... mosquitto version 2.0.15 running
[bridge] plugin loaded, bridge channel qnx_mqtt_bridge, cmd topic qnx/cmd/#
```

(The exact `[bridge]` line comes from the plugin's init log; see
`plugin/qnx_bridge.c`.)

- **PASS:** broker stays up, port 1883 listens, plugin init line
  present.
- **FAIL:** broker exits, or the plugin line is missing / reports an
  error — check the `plugin` path in mosquitto.conf and that the
  Mosquitto port libraries are installed on the target.

## Step 4 — Start the demo app [PENDING TOOLCHAIN]

On the target console (second shell, or backgrounded):

```sh
/usr/local/bin/qnx_sensor
```

Expected console output, line by line:

```
[sensor] channel qnx_demo_sensor attached
[sensor] HELLO sent to bridge (registered as qnx_demo_sensor)
[sensor] telemetry every 2 s on qnx/sensor/temp
[sensor] telemetry qnx/sensor/temp {"temp_c":24.5,"unit":"C"}
[sensor] telemetry qnx/sensor/temp {"temp_c":24.7,"unit":"C"}
```

(First temperature value depends on when the app started — the sine
drift starts at 24.5 °C and moves 23.0–26.0 °C over 60 s. The value
itself is not the check; the check is one line every 2 s, valid JSON,
`temp_c` inside 23.0–26.0.)

- **PASS:** HELLO line, then a telemetry line every ~2 s.
- **FAIL:** "waiting for bridge ..." for more than 60 s then exit —
  the broker/plugin is not up (go back to step 3). No telemetry lines
  after HELLO — the bridge is not forwarding (check the broker log
  for publish errors).

## Step 5 — Watch telemetry from the host [PENDING TOOLCHAIN]

On the host:

```sh
mosquitto_sub -h <target-ip> -t 'qnx/sensor/#' -v
```

Expected, one line every ~2 s:

```
qnx/sensor/temp {"temp_c":24.5,"unit":"C"}
qnx/sensor/temp {"temp_c":24.7,"unit":"C"}
```

- **PASS:** lines arrive at the 2 s cadence, valid JSON.
- **FAIL:** nothing arrives — check the sub actually connected
  (broker log shows the client), and that step 4 shows telemetry
  being sent.

## Step 6 — Send commands from the host [PENDING TOOLCHAIN]

Keep the step-5 subscriber running. From another host shell:

```sh
mosquitto_pub -h <target-ip> -t qnx/cmd/sensor/set_interval -m 5
mosquitto_pub -h <target-ip> -t qnx/cmd/sensor/led -m on
mosquitto_pub -h <target-ip> -t qnx/cmd/sensor/ping -m ''
```

Expected on the **target console** (qnx_sensor stdout):

```
[sensor] command qnx/cmd/sensor/set_interval args="5"
[sensor] telemetry interval set to 5 s
[sensor] command qnx/cmd/sensor/led args="on"
[sensor] LED ON
[sensor] command qnx/cmd/sensor/ping args=""
[sensor] ping -> pong
```

Expected on the **host subscriber** (step 5):

```
qnx/sensor/ack set_interval 5 ok
qnx/sensor/ack led on ok
qnx/sensor/status {"status":"pong","led":"on"}
```

And after `set_interval 5`, the `qnx/sensor/temp` lines slow to one
every 5 s.

- **PASS (per command):** the command line appears on the target
  console AND its ack/status line appears on the host subscriber.
  All three commands must pass.
- **FAIL:** a command appears on the target but no ack on the host —
  the ack path (bridge inbound queue → TICK publish) is broken. A
  command never appears on the target — the outbound queue/routing
  is broken (check the `qnx/cmd/#` filter and the registry: the
  HELLO must have registered `qnx_demo_sensor`).

Extra checks (same session):

```sh
mosquitto_pub -h <target-ip> -t qnx/cmd/sensor/set_interval -m 99
# -> clamped: "[sensor] telemetry interval set to 60 s", ack "set_interval 60 ok"
mosquitto_pub -h <target-ip> -t qnx/cmd/sensor/set_interval -m 0
# -> clamped: interval 1 s, ack "set_interval 1 ok"
mosquitto_pub -h <target-ip> -t qnx/cmd/sensor/led -m blink
# -> "[sensor] LED bad arg: "blink" (want on|off)", ack "led: bad arg ..."
mosquitto_pub -h <target-ip> -t qnx/cmd/sensor/bogus -m x
# -> "[sensor] unknown command: bogus", ack "unknown command: bogus"
mosquitto_pub -h <target-ip> -t qnx/cmd/sensor/set_interval -m 2
# -> back to the 2 s cadence
```

- **PASS:** clamps and error acks behave exactly as above.
- **FAIL:** any crash, hang, or missing ack — the demo must never die
  on a bad command.

## Step 7 — Clean shutdown [PENDING TOOLCHAIN]

On the target: Ctrl-C the demo, then stop the broker (Ctrl-C).

Expected from the demo:

```
[sensor] shutting down
```

- **PASS:** both processes exit cleanly, no hang, no segfault.
- **FAIL:** a hang on exit — note it; the worker-thread join / pulse
  wakeup path is the suspect (see DESIGN.md shutdown notes).

## Whole-demo PASS criteria

All of steps 0–7 PASS, with no FAIL at any step. The demo is the
evidence: a stranger following this file should see the same lines
in the same order.

## What is NOT covered here

- aarch64 target (stretch goal, untested).
- TLS / auth / retained messages (v1 non-goals).
- Broker restart while the demo runs (demo re-looks the bridge up
  after a failed telemetry send; full restart behaviour is manual
  follow-up, not a PASS/FAIL gate for v1).

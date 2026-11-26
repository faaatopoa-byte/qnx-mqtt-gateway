# Build an MQTT ↔ QNX Native Messaging Gateway

## A codelab: bridge a Mosquitto broker to QNX message passing with a plugin, and prove it works in QEMU

**Repo:** https://github.com/faaatopoa-byte/qnx-mqtt-gateway
**Tested with:** QNX SDP 8.0, Mosquitto 2.0.15 (official QNX port), QEMU x86_64
**Reading time:** about 20 minutes. **Hands-on time:** about 3 to 4 hours if you start from a fresh SDP install, under an hour if the toolchain is already in place.

---

## What you will build

A small Mosquitto broker plugin that connects MQTT topics to QNX native message passing, plus a demo sensor app that talks to it. When you finish, you will be able to do this from any machine on the network:

```sh
mosquitto_sub -h <target-ip> -t 'qnx/sensor/#' -v
mosquitto_pub -h <target-ip> -t qnx/cmd/sensor/led -m on
```

and watch a native QNX process answer over `MsgSend`/`MsgReply`, with the reply coming back as an MQTT message. No TCP socket inside the QNX app, no polling loop in the broker, no extra daemon in between. (In the QEMU demo in step 6 the broker only listens on loopback, so `<target-ip>` there is `127.0.0.1` from inside the guest; the commands above are the form you use against a real networked device.)

The moving parts:

- **The broker** is the official Mosquitto 2.0.15 QNX port (`github.com/qnx-ports/mosquitto`). We do not port it. It already exists and already works.
- **The plugin** (`qnx_bridge.so`) is our code. It hooks `MOSQ_EVT_MESSAGE` and `MOSQ_EVT_TICK`, owns a worker thread that does all blocking QNX IPC, and keeps two bounded queues between the broker thread and the worker thread.
- **The demo app** (`qnx_sensor`) is a native QNX process. It registers with the bridge with a HELLO message, sends temperature telemetry every 2 seconds, and answers three commands: `set_interval`, `led`, and `ping`.

The finished system is 2,826 lines of C with 241 host unit tests, and it was verified end to end in QEMU: 115 telemetry publishes, all three commands answered, the pong payload `{"status":"pong","led":"on"}` delivered, zero NAKs, zero dropped messages, broker alive for the full 600-second test.

---

## Prerequisites

- A Linux x86_64 host. The QNX toolchain and the Mosquitto port recipes only support a Linux host.
- A myQNX account with the **SDP 8.0 Non-Commercial** licence deployed to it. The QNX Software Center installer is behind a login wall: an unauthenticated download request redirects to the myQNX login page. You need your own credentials twice, once to download the installer and once inside the Software Center to sync the licence and fetch the SDP packages. Never put those credentials in a script or a chat message.
- QEMU (`qemu-system-x86_64`, version 8.2.2 was used here). Install it before you start (`sudo apt-get install -y qemu-system-x86` on Ubuntu); the boot step needs it.
- `git`, `make`, `gcc`, `ssh`, `scp`, and the Mosquitto clients (`mosquitto_pub`, `mosquitto_sub`, any 2.x).
- About 600 MB of free RAM before you boot QEMU. The guest runs with 256 MB (`-m 256`), and the standing rule in this project is to boot only when at least 600 MB is free. Check with `free -m` before you start the emulator.

---

## Step 1: install the SDP 8.0 toolchain

1. Log in to your myQNX account and download the QNX Software Center 2.0.4 installer for Linux 64-bit:

   ```sh
   # qnx-setup-2.0.4-202501021438-linux.run  (~100 MB)
   # Official download page:
   # https://www.qnx.com/download/group.html?programid=29178
   ```

   No checksum is published for the installer, so integrity rests on the authenticated HTTPS download from qnx.com itself.

2. Run the installer and use the Software Center to install SDP 8.0 (baseline `com.qnx.qnx800` v8.0.0.00141T202311271501L or patchset 8.0.6). The multi-gigabyte part is the SDP packages, downloaded by the Software Center after licence sync.

   **Version note:** this codelab was tested with SDP 8.0 (baseline `com.qnx.qnx800` v8.0.0.00141T202311271501L or patchset 8.0.6), QEMU 8.2.2, and Mosquitto 2.0.15. If the Software Center gives you a newer patchset, most steps should still work — SDP 8.0 patchsets are backward-compatible — but package names, file locations, or behaviour may differ slightly. If a step fails, check your versions first and compare them with the versions listed here.

3. Set up the environment by hand. A full SDP install may provide `qnxsdp-env.sh`; a hand-assembled one, like the one used for this project, does not — the file exists nowhere under `~/qnx`, so do not rely on sourcing it. Set the variables yourself:

   ```sh
   export QNX_HOST=$HOME/qnx/qnx800/host/linux/x86_64
   export QNX_TARGET=$HOME/qnx/qnx800/target/qnx
   export PATH=$QNX_HOST/usr/bin:$PATH
   qcc -V               # must list gcc_ntox86_64
   echo $QNX_TARGET     # must be non-empty
   ```

   Run these exports in every new shell before you build anything: the toolchain only works while `QNX_HOST`, `QNX_TARGET`, and the `PATH` entry are set.

   **Expected result:** `qcc -V` lists `gcc_ntox86_64` as the default target, and `$QNX_TARGET` points at your SDP target tree.

   **Partial installs:** a partial SDP install can look complete while missing the networking headers (`sys/socket.h`, `netinet/in.h`, `arpa/inet.h`, `netdb.h`) and libraries (`libsocket.so`, `libpthread.so`, `libm.so`). The plugin and the demo app build around those gaps, but the Mosquitto port itself cannot be built without them. If your install is partial, go back to the Software Center and add the missing libc-dev and networking packages before step 2. Do not hand-roll socket headers.

---

## Step 2: Build Mosquitto from the official QNX port

Do not write your own port. The official one is maintained, tested, and already adapted to QNX. Clone the two repos (pin the `qnx_v2.0.15` branch) and build with the port's own CMake recipe, run directly:

```sh
git clone https://github.com/qnx-ports/build-files.git
git clone https://github.com/qnx-ports/mosquitto.git
cd mosquitto && git checkout qnx_v2.0.15 && cd ..
```

**Why not the build command in the port's README?** The README says to run `source ~/qnx800/qnxsdp-env.sh` and then `BUILD_TESTING=ON QNX_PROJECT_ROOT="$(pwd)/mosquitto" make -C build-files/ports/mosquitto install -j4`. That only works on a full SDP install: it needs `qnxsdp-env.sh` and the QNX recursive-make files (`qconfig.mk`, `recurse.mk`, `qtargets.mk`). A hand-assembled SDP has neither — no `qnxsdp-env.sh` anywhere under `~/qnx`, and zero `*.mk` files in the whole SDP tree (check with `find ~/qnx -name '*.mk' | wc -l`). The recipe below runs the same CMake configuration the port's `common.mk` runs, directly, with the same flags. It changes no source and no result; it only skips the recursive-make wrapper.

First, check the math prerequisite. The broker needs `math.h` and `libm` (`src/sys_tree.c` calls `fabs()` and `exp()`):

```sh
ls "$QNX_TARGET/usr/include/math.h" "$QNX_TARGET/x86_64/lib/libm.so.3"
```

If either file is missing, fetch the package that provides them from the QNX Software Center first (File Lookup for `libm.so.3`), install it into your SDP tree, and re-check. Without it the build stops at `mosquitto/src/session_expiry.c: fatal error: math.h: No such file or directory`.

Then configure, build, and install, from the directory holding both clones (`PORTS` is the port recipe directory inside `build-files`):

```sh
export QNX_HOST=$HOME/qnx/qnx800/host/linux/x86_64
export QNX_TARGET=$HOME/qnx/qnx800/target/qnx
export PATH=$QNX_HOST/usr/bin:$PATH

PORTS="$(pwd)/build-files/ports/mosquitto"
cmake -S mosquitto -B build \
  -DCMAKE_TOOLCHAIN_FILE="$PORTS/qnx.nto.toolchain.cmake" \
  -DCMAKE_INSTALL_PREFIX="$QNX_TARGET" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_BINDIR="$QNX_TARGET/x86_64/usr/local/bin" \
  -DCMAKE_INSTALL_INCLUDEDIR="$QNX_TARGET/usr/local/include" \
  -DCMAKE_INSTALL_LIBDIR="$QNX_TARGET/x86_64/usr/local/lib" \
  -DCMAKE_INSTALL_SBINDIR="$QNX_TARGET/x86_64/usr/local/sbin" \
  -DCMAKE_MODULE_PATH="$PORTS" \
  -DEXTRA_CMAKE_C_FLAGS="-g -D_QNX_SOURCE" \
  -DEXTRA_CMAKE_CXX_FLAGS="-g -D_QNX_SOURCE" \
  -DEXTRA_CMAKE_LINKER_FLAGS="-lsocket" \
  -DCPUVARDIR=x86_64 \
  -DGCC_VER=12.2.0 \
  -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
  -DWITH_CJSON=OFF \
  -DWITH_ADNS=OFF \
  -DWITH_LIB_CPP=OFF \
  -DCMAKE_CXX_COMPILER_WORKS=TRUE \
  -DDOCUMENTATION=OFF \
  -DBUILD_TESTING=ON
cd build && make -j2 all && make install && cd ..
```

Notes on the flags: `-j2` because a shared machine has little CPU (use `-j4` on a real machine); `GCC_VER=12.2.0` must match your toolchain's GCC (check with `qcc -V`); `-DWITH_LIB_CPP=OFF` skips the C++ library, which a hand-assembled SDP does not have; `-DWITH_CJSON=OFF` and `-DDOCUMENTATION=OFF` match the port's own `common.mk`. The build prints OpenSSL 3.0 ENGINE deprecation warnings from upstream code; those are harmless.

What that recipe does for you:

- CMake runs directly with the same configuration the port's `common.mk` runs (`qnx.nto.toolchain.cmake`, `-DWITH_CJSON=OFF`, `-DDOCUMENTATION=OFF`, install prefix `$QNX_TARGET`). The only thing skipped is the recursive-make wrapper, which needs files a hand-assembled SDP does not have.
- `-D_QNX_SOURCE` and `-lsocket` (the port's `common.mk` adds both).
- epoll disabled on QNX, so the broker uses POSIX `poll()` (`mux_poll.c`). This matters for step 3: the broker is a single-threaded `poll()` loop.
- TLS on, via the OpenSSL libraries in the SDP target tree.
- The plugin headers `mosquitto_plugin.h` and `mosquitto_broker.h` are installed with the port. The gateway code is written against those headers, and the copies in this repo under `third-party/mosquitto/` are byte-identical to the `qnx_v2.0.15` branch.

**Expected result:** the broker lands at `$QNX_TARGET/x86_64/usr/local/sbin/mosquitto`, `mosquitto_pub` and `mosquitto_sub` at `$QNX_TARGET/x86_64/usr/local/bin/`, `libmosquitto.so.2.0.15` at `$QNX_TARGET/x86_64/usr/local/lib/`, and the two plugin headers at `$QNX_TARGET/usr/local/include/`. Verify with `ls` on those paths before you go on — if the broker binary is not in `sbin/`, the install step did not run.

---

## Step 3: the bridge plugin: two queues and a worker thread

The design matters more than the code here. Two rules decide whether the plugin works at all.

### Why the broker must never block

The Mosquitto broker runs a single-threaded `poll()` loop. Any hook callback (`MOSQ_EVT_MESSAGE`, `MOSQ_EVT_TICK`, and the rest) runs on that thread. If a hook calls `MsgSend()` and the QNX app on the other end is slow, the whole broker stops answering MQTT clients until the reply comes back. So the rule is simple: **hooks only touch queues; all blocking IPC lives in the plugin's own worker thread.**

### Why the worker thread must never publish

The tempting fix is to have the worker thread call `mosquitto_broker_publish()` directly when an answer arrives from a QNX app. That is a data race in Mosquitto 2.0.15, and we verified it against the source, not just the docs:

- `mosquitto_broker_publish()` in `src/plugin_public.c` appends to the list `db.plugin_msgs` with `DL_APPEND`. There is no mutex anywhere in that file.
- The main loop drains the same list with `DL_FOREACH_SAFE` / `DL_DELETE` in `queue_plugin_msgs()` (`src/loop.c`), at the start of every iteration of `mosquitto_main_loop`.
- A `DL_APPEND` from a worker thread racing a `DL_DELETE` from the main thread over the same doubly-linked list, with no lock, corrupts the list.

The docs say nothing about thread safety either way, so the absence of a warning is not permission. The safe pattern, and the one every official example plugin uses, is: the worker thread enqueues the message, and the publish happens in the `MOSQ_EVT_TICK` callback, which runs on the main loop thread. All three official example plugins that publish (`mosquitto_payload_size_stats.c`, `mosquitto_client_lifetime_stats.c`, `mosquitto_plugin_event_stats.c`) publish from `MOSQ_EVT_TICK` and nowhere else.

### The layout

```
  MQTT client
        |  publish qnx/cmd/sensor/led on
        v
  +---------------- mosquitto broker (official QNX port 2.0.15) -----+
  |  MOSQ_EVT_MESSAGE (qnx/cmd/# only)                                |
  |     -> copies topic+payload into the bounded outbound queue       |
  |  MOSQ_EVT_TICK                                                    |
  |     -> drains inbound queue via mosquitto_broker_publish()        |
  |  worker thread (blocking IPC lives ONLY here)                     |
  |     -> MsgReceive() on qnx_mqtt_bridge, MsgSend() commands out    |
  +-------------------------------------------------------------------+
        | MsgSend / MsgReply (QNX native message passing)
        v
  +---------------- qnx_sensor (native QNX demo app) -----------------+
```

The wire format between bridge and apps is one framed message, max 4096 bytes (`common/qnx_msg.h`):

```
magic 'QMBR' | topic_len | payload_len | topic bytes | payload bytes
```

Channels: `qnx_mqtt_bridge` (owned by the plugin worker thread) and `qnx_demo_sensor` (owned by the demo app). Registration is a HELLO message, topic `__hello__`, payload = the app's own channel name. The bridge keeps a fixed registry (max 8 apps) and routes commands by longest-prefix match on the registered topic prefixes.

### Build it — host first, then QNX

The host build comes first: it creates `plugin/build/host-include/`, which the QNX plugin build needs, and it proves the logic before you touch the target.

```sh
git clone https://github.com/faaatopoa-byte/qnx-mqtt-gateway
cd qnx-mqtt-gateway
cd plugin && make -f Makefile.host && cd ..
cd test && ./run_tests.sh        # must pass before the code is "done"
cd ../demo && make -f Makefile.host && cd ..   # builds demo/qnx_sensor_host
```

**Expected result:** `run_tests.sh` prints **241 checks, 0 failures**. Do not proceed to the target with a failing host test. The host build uses a stub IPC backend (`plugin/qnx_ipc_host.c`, a Unix-domain socket with the same semantics) so the framing, queues, registry, and routing can be tested on Linux. The host stub is never shipped to the target; it exists only so the logic is provable on Linux.

Then the QNX builds, from the repo root:

```sh
cd plugin && make -f Makefile.qnx && cd ..
cd demo && make -f Makefile.qnx && cd ..
```

The plugin builds with:

```sh
qcc -Vgcc_ntox86_64 -shared -fPIC -O2 -Wall -Wextra -std=c11 \
  -D_QNX_SOURCE -Ibuild/host-include -I../common \
  -c qnx_bridge.c -o qnx_bridge.o
```

and links without `-lmosquitto`: the broker resolves the plugin symbols at load time. The demo app builds with `qcc -Vgcc_ntox86_64` and no extra libraries — no `-lsocket`, `-lpthread`, or `-lm`: in this SDP the needed symbols resolve from `libc.so`, and the demo's sine drift is a Taylor approximation (`demo_sin`), so it needs no `libm` either.

Verify the plugin exports before you go further:

```sh
ntox86_64-nm -D plugin/qnx_bridge.so | grep mosquitto_plugin_
# must list: mosquitto_plugin_version, mosquitto_plugin_init,
#            mosquitto_plugin_cleanup
```

**Expected result:** `qnx_bridge.so` and `qnx_sensor` exist, built with zero warnings, and the `nm` check lists all three symbols.

### Four build fixes you will probably hit

These are the exact fixes applied on the first real `qcc` run, in case you hit the same walls:

1. **`TimerTimeout` signature.** SDP 8.0 takes `const _Uint64t *ntime` (nanoseconds), not `struct timespec *`. Pass `(uint64_t)timeout_ms * 1000000ULL`.
2. **`-D_QNX_SOURCE`.** With strict `-std=c11`, QNX extensions (`SIGEV_PULSE_INIT`, `SIGEV_PULSE_PRIO_INHERIT`, `usleep`) are hidden without it. This matches the Mosquitto port's own `common.mk`.
3. **No `math.h` / `libm`.** If your target tree lacks them, the sine drift in the demo is a Taylor approximation (`demo_sin`, error ~1e-7) and `-lm` is dropped from the demo Makefile.
4. **No `-lsocket` / `-lpthread`.** If the target tree lacks those libraries, link without them; the needed symbols resolve from `libc.so`.

---

## Step 4: the sensor app: native messaging, and one lesson about MsgSend

The demo app is deliberately small: `name_attach("qnx_demo_sensor")`, a HELLO to the bridge on start, telemetry every 2 seconds, and three commands.

- Telemetry: topic `qnx/sensor/temp`, payload `{"temp_c":23.5,"unit":"C"}`, with a slow sine drift from 23.0 to 26.0 °C over 60 seconds. Deterministic, no `rand()` seeding surprises.
- `set_interval <secs>`: change the telemetry period, clamped to 1 to 60.
- `led <on|off>`: simulated LED state, printed and reported.
- `ping`: replies `pong` as telemetry topic `qnx/sensor/status`.

Two design details are worth copying:

- **No cross-send deadlock.** The demo's receive thread only receives (and replies); a separate sender thread only sends telemetry. The ping `pong` travels inside the command reply itself, never as a second nested `MsgSend` while the bridge is waiting for the reply.
- **Bounded queues, drop-oldest.** The outbound queue holds 64 messages; overflow drops the oldest and counts it. A slow QNX app can never wedge the broker.

### The MsgSend lesson

`MsgSend()` returns only the **status** the server passed to `MsgReply()`. It never returns the length of the reply. The QNX docs say it plainly: "Any other return value is the status from MsgReply()", and "The number of bytes transferred is the minimum of that specified by both the sender and the receiver." The kernel transfers the status value to the client; the byte count is never reported back.

The only official way to convey a reply length is to encode it in the status value itself, which is exactly what the documented resource-manager `read()` example does: `MsgReply(ctp->rcvid, nbytes, ...)` and the client reads the byte count out of the `MsgSend` return value.

The first version of the bridge code filled in `*reply_len` only for framed replies of 12 bytes or more (the `QMBR` magic plus the two length fields). The bridge's reply to a HELLO is a single status byte, so `reply_len` came back as 0, and the sensor treated every HELLO as a failure even though the bridge had accepted it. The fix is to drop the length requirement entirely: success means the `MsgSend` returned OK **and** the status byte is 0. Do not write code that expects `MsgSend` to tell you how many bytes came back. It will not.

---

## Step 5: assemble a minimal IFS image

A full QNX target image is heavy. For this demo you want the smallest image that still boots, starts the network stack, and runs the broker. The buildfile used here is `ifs/minimal.build`. The pieces that matter, and why each one is there:

- **Boot:** `[virtual=x86_64,multiboot]` with `startup-x86 -D8250..115200 -zz -f 0,0,1000M` and `procnto-smp-instr`. Serial console on `devc-ser8250`, reopened as `/dev/ser1`.
- **`mount_shm` and `/dev/shmem`.** Shared memory lives at `/dev/shmem` on QNX, not `/dev/shm` (that is the Linux path). `/dev/shmem` is built into `procnto`, and the startup script marks it so the broker and the plugin can use it.
- **Order:** `slogger2` first, then `random`, then `io-sock`. That is the stock order, and the boot depends on it.
- **`qcrypto.conf`.** The broker needs the crypto plugin to load, and it will not load without its config. The buildfile inlines it:

  ```
  [uid=0 gid=0 perms=0444] qcrypto.conf={
  openssl-3 tags=*
  }
  [type=link] /etc/qcrypto.conf=/proc/boot/qcrypto.conf
  ```

  plus the plugin binary `lib/dll/qcrypto-openssl-3.so` in the image, with `LD_LIBRARY_PATH=/proc/boot` so it is found. Without this file the broker fails to start, and the error does not point at the missing config, which cost a full debug round to find.
- **The broker config** (`ifs/mosquitto.conf`) is baked into the image at `/proc/boot/mosquitto.conf`:

  ```conf
  listener 1883 127.0.0.1
  allow_anonymous true
  user root
  plugin /proc/boot/qnx_bridge.so
  plugin_opt_qnx_cmd_topic qnx/cmd/#
  plugin_opt_qnx_bridge_name qnx_mqtt_bridge
  plugin_opt_qnx_log 1
  ```

  Note `user root`: the broker refuses to run as an unprivileged user in this image, and an earlier test failed on exactly that.
- **The startup script** starts the broker, then the sensor, then subscribes three local clients (temp, ack, status) and sends the three test commands itself, so the boot log alone proves the whole loop. It ends with `pidin` and `GATEWAY TEST SEQUENCE DONE`.

### Build the helpers first

The buildfile pulls three small helper binaries from `test/` — `mark`, `mount_shm`, and `test_timer_timeout` — and there is no Makefile for them. Build each one from its source (the exact command is in each file's header comment):

```sh
export QNX_HOST=$HOME/qnx/qnx800/host/linux/x86_64
export QNX_TARGET=$HOME/qnx/qnx800/target/qnx
export PATH=$QNX_HOST/usr/bin:$PATH
cd test
qcc -Vgcc_ntox86_64 -O2 -Wall -Wextra -std=c11 -D_QNX_SOURCE -o mark mark.c
qcc -Vgcc_ntox86_64 -O2 -Wall -Wextra -std=c11 -D_QNX_SOURCE -o mount_shm mount_shm.c
qcc -Vgcc_ntox86_64 -O2 -Wall -Wextra -std=c11 -D_QNX_SOURCE -o test_timer_timeout test_timer_timeout.c
cd ..
```

### Edit the search paths, then build the image

The buildfile's `[search=...]` line hardcodes the original author's directories. Edit it so the four workspace paths at the end of the line point at your clone — `plugin`, `demo`, `ifs`, and `test` under your repo root (for example, if you cloned to `~/qnx-mqtt-gateway`, those four entries become `~/qnx-mqtt-gateway/plugin`, `~/qnx-mqtt-gateway/demo`, `~/qnx-mqtt-gateway/ifs`, and `~/qnx-mqtt-gateway/test`).

Then build the image from the repo root. Always feed `mkifs` an empty stdin (`</dev/null`): a bare `mkifs` blocks waiting for input, and never run `mkifs -V` (it dumps binary output).

```sh
mkifs ifs/minimal.build minimal-ifs.bin </dev/null
```

**Expected result:** `mkifs` exits 0 and `minimal-ifs.bin` is about 20,667,636 bytes. Do not compare checksums between two builds: `mkifs` embeds a timestamp, so the same buildfile gives a different md5 every time. Size is the real check.

---

## Step 6: boot in QEMU and run the verification

Install QEMU first if it is not already there (on Ubuntu: `sudo apt-get update && sudo apt-get install -y qemu-system-x86`; version 8.2.2 was used here — check with `qemu-system-x86_64 --version`). The update step matters on a fresh machine: without it the install fails with "Unable to locate package". The update can take several minutes on a slow connection. Note: QEMU lives in `/usr`, so it disappears on a VM that wipes `/usr` on restart; if `qemu-system-x86_64` is suddenly "not found", just reinstall it.

Check the load gate before you boot — one heavy job at a time on a shared machine:

```sh
uptime    # load average (1 min) must be below 2.0
free -m   # MemAvailable must be at least 600 MB
```

Boot the image in QEMU (256 MB guest RAM) and capture the whole session to a log. The `timeout 600` ends the run after 10 minutes; exit code 124 from `timeout` is the normal end of a good run, not a failure.

```sh
timeout 600 qemu-system-x86_64 -m 256 -smp 1 -cpu max -machine pc -kernel minimal-ifs.bin -nographic -vga none -monitor none > qnx-boot.log 2>&1
echo "qemu exit: $?"   # 124 = timeout ended the run; that is the normal end
```

The startup script runs the whole test by itself; your job is to read the log and check it against this list.

**Expected boot lines:**

```
MINIMAL IFS BOOT OK
... mosquitto version 2.0.15 starting
... mosquitto version 2.0.15 running
GATEWAY TEST SEQUENCE DONE
```

**Expected sensor lines on the target console:**

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

**Expected lines on the subscriber:**

```
qnx/sensor/temp {"temp_c":24.5,"unit":"C"}
qnx/sensor/ack set_interval 5 ok
qnx/sensor/ack led on ok
qnx/sensor/status {"status":"pong","led":"on"}
```

After `set_interval 5`, the `qnx/sensor/temp` lines slow to one every 5 seconds. That change in cadence is itself a check: it proves the command reached the app and changed its behaviour, not just that it was logged.

### The verification checklist

This is the list the v10 run passed. Every item must hold; a single miss means the gateway is not proven.

| # | Check | v10 result |
|---|-------|------------|
| 1 | Broker starts, `mosquitto version 2.0.15 running` in the log | pass |
| 2 | Broker stays alive for the full 600-second test | pass |
| 3 | Telemetry publishes on `qnx/sensor/temp`, one every 2 s at first | 115 publishes |
| 4 | `set_interval 5` received, interval changes to 5 s | pass |
| 5 | `led on` received, `LED ON` printed | pass |
| 6 | `ping` received, `ping -> pong` printed | pass |
| 7 | Ack `qnx/sensor/ack set_interval 5 ok` published | pass |
| 8 | Ack `qnx/sensor/ack led on ok` published | pass |
| 9 | Pong payload `{"status":"pong","led":"on"}` delivered on `qnx/sensor/status` | pass |
| 10 | Zero NAKs in the log | 0 |
| 11 | Zero `drop outbound: no app registered` in the log | 0 |
| 12 | Host unit tests still green after the target run | 241/241 |

### Extra checks (same session, from the guest console)

The guest has no network interface in this setup — `io-sock` brings up loopback only, and the QEMU command line has no `-netdev` — so there is no `<target-ip>` to reach from the host. Run these from the guest console instead: after `GATEWAY TEST SEQUENCE DONE` the startup script leaves you at a login shell (`sh -l`) on the serial console, and the session runs until the 600-second timeout ends it. Type them there, against loopback:

```sh
mosquitto_pub -h 127.0.0.1 -t qnx/cmd/sensor/set_interval -m 99
# -> clamped: "[sensor] telemetry interval set to 60 s", ack "set_interval 60 ok"
mosquitto_pub -h 127.0.0.1 -t qnx/cmd/sensor/set_interval -m 0
# -> clamped: interval 1 s, ack "set_interval 1 ok"
mosquitto_pub -h 127.0.0.1 -t qnx/cmd/sensor/led -m blink
# -> "[sensor] LED bad arg: "blink" (want on|off)", ack "led: bad arg ..."
mosquitto_pub -h 127.0.0.1 -t qnx/cmd/sensor/bogus -m x
# -> "[sensor] unknown command: bogus", ack "unknown command: bogus"
mosquitto_pub -h 127.0.0.1 -t qnx/cmd/sensor/set_interval -m 2
# -> back to the 2 s cadence
```

The demo must never die on a bad command. Any crash, hang, or missing ack on these is a fail.

The manual variant in `docs/TEST-PLAN.md` (copy files to a running target with `scp`, start the broker and the demo by hand) needs a networked target with an SSH server; it does not apply to this loopback-only QEMU setup.

### How the run ends

The session ends when the 600-second `timeout` kills QEMU (exit code 124) — that is the normal end of a good run, not a crash. There is no clean shutdown step in this procedure: the startup script runs the test, prints `GATEWAY TEST SEQUENCE DONE`, and leaves a shell; the timeout ends the session. The demo's shutdown path (`[sensor] shutting down` on Ctrl-C) is in the code (`demo/qnx_sensor.c`) but is not exercised by this run.

---

## Troubleshooting: three real bugs and what each one taught

Each one stopped a real test run, and each fix is in the code you just built.

### 1. The fake HELLO NAK (MsgSend reply length)

**Symptom:** the sensor reported every HELLO as failed, even though the bridge had accepted it.
**Cause:** `qnx_ipc_send` only filled in `*reply_len` for framed replies of 12 bytes or more. The bridge replies to a HELLO with a single status byte, so `reply_len` came back 0, and the sensor required `reply_len >= 1`.
**Fix:** drop the length requirement. Success is `MsgSend` returning OK and the status byte being 0. See step 4 for why `MsgSend` can never report a reply length: the kernel only transfers the status value. Check the return-value contract of the IPC call before writing the check around it.

### 2. The prefix that did not match

**Symptom:** three `drop outbound: no app registered` lines in the broker log, and no command ever reached the sensor.
**Cause:** the plugin stripped `qnx/cmd/demo/sensor/` from the channel name when it registered the app, but the sensor's commands arrive under `qnx/cmd/sensor/`. The registry had the wrong prefix, so longest-prefix routing found nothing.
**Fix:** the HELLO registers the app as `"qnx_demo_sensor qnx/cmd/sensor/"`, channel name plus the prefix the commands actually use. When routing drops a message, print the prefix it tried to match; the registered prefix has to match the topic the commands arrive on, not the one you assumed.

### 3. The ack that was never published

**Symptom:** commands reached the sensor and the sensor answered, but no ack ever appeared on the subscriber.
**Cause:** the worker thread's `drain_outbound` only logged the ack. It never put it in the inbound queue, so nothing ever reached the `MOSQ_EVT_TICK` publish path.
**Fix:** after unpacking the ack, push it into the inbound queue (`bridge_queue_push(&st->inbound, ...)`), and let TICK publish it. The wrong fix would have been to call `mosquitto_broker_publish()` directly from the worker thread; that is the data race described in step 3. The queue plus TICK is the only safe pattern in 2.0.15. If the ack does not reach the subscriber, the path is broken no matter how clean the log looks.

### Two more traps from earlier in the project

- **TimerTimeout and SIGEV_PULSE.** An earlier version armed `TimerTimeout` with a pulse event and got EINVAL. The docs for QNX 6.1 through 7.1 explicitly recommend SIGEV_PULSE and list only two documented causes for EINVAL (invalid clock ID, or TIMER_TOLERANCE with a non-NULL notify), neither of which applied. The cause on SDP 8.0 is still open; the working fix is to pass NULL for notify, which the docs recommend anyway ("You'll almost always want to use an event of type SIGEV_UNBLOCK"). Do not quote the claim that SIGEV_PULSE is always EINVAL as documented fact. It is not in the docs.
- **The missing `qcrypto.conf`.** The broker would not start until the crypto plugin config was in the image (step 5). If your broker dies at startup with no clear error, check that file first.

---

## What this does not cover

- No TLS, no auth or ACL plugin, no MQTT v5 properties, no retained messages. Those are deliberate non-goals for this version.
- aarch64 is a stretch goal. The Makefiles carry a commented `gcc_ntoaarch64le` variant that has never been run.
- Broker restart while the demo runs. The demo re-looks the bridge up after a failed telemetry send, but full restart behaviour is manual follow-up, not part of the pass criteria.

---

## Where to go next

- Point the bridge at a real device instead of the demo sensor: any native QNX app can register with a HELLO and a topic prefix, and the registry holds up to 8 apps.
- Add a second app and watch longest-prefix routing sort the commands between them.
- Try the `MsgReceivev` variant of the TimerTimeout test to settle the open pulse question from the troubleshooting section.
- Read the full contract in `DESIGN.md` before changing any interface, and the full test procedure in `docs/TEST-PLAN.md` before re-running the demo.

**Repo:** https://github.com/faaatopoa-byte/qnx-mqtt-gateway
**Licence:** MIT for the bridge plugin, the demo app, and the docs. Mosquitto and its official QNX port keep their own licences.

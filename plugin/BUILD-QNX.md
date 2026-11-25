# BUILD-QNX.md — building the plugin for QNX (toolchain NOT here yet)

> **Update, 25 Nov 2026:** the blockers in section 5 were resolved
> after this note was written. The QEMU v10 run passed in full, see
> the Status section in `README.md`. The rest of this file is kept
> as the record of the first qcc run.

> **Status: VERIFIED 25 Nov 2026 ~07:23 WIB.** `qcc -V` lists
> `gcc_ntox86_64` (default); `make -C plugin -f Makefile.qnx` and
> `make -C demo -f Makefile.qnx` both succeed with zero warnings;
> `nm -D` confirms `mosquitto_plugin_version/init/cleanup` in
> `qnx_bridge.so`; host unit tests still 236 checks, 0 failures.
> Fixes applied this run (see §4): TimerTimeout signature, `-D_QNX_SOURCE`,
> local Mosquitto headers, no `-lmosquitto`, no `-lsocket/-lpthread/-lm`.
> The QEMU end-to-end test is still blocked (see §5) — the plugin has
> never been loaded by a real broker.

## Prerequisites (all must be true first)

1. SDP 8.0 installed, e.g. `~/qnx/qnx800/` populated (baseline
   `com.qnx.qnx800` v8.0.0.00141T202311271501L or patchset 8.0.6).
   **Caveat (25 Nov 2026): this install is PARTIAL** — qcc/gcc/binutils,
   microkernel core/support, fs.base, tools.cpp only. Missing from the
   target tree: `sys/socket.h`, `netinet/in.h`, `arpa/inet.h`,
   `netdb.h`, `net/if.h`, `math.h`, `complex.h`, and the libs
   `libsocket.so`, `libpthread.so`, `libm.so`. That blocks building the
   Mosquitto port itself (see §5); the plugin/demo build around it.
2. The SDP environment set so `qcc` is on `PATH` and `QNX_TARGET`
   is set. **This install has no `qnxsdp-env.sh`** — set by hand:
   ```sh
   export QNX_HOST=$HOME/qnx/qnx800/host/linux/x86_64
   export QNX_TARGET=$HOME/qnx/qnx800/target/qnx
   export PATH=$QNX_HOST/usr/bin:$PATH
   qcc -V               # must list gcc_ntox86_64
   ```
3. The official Mosquitto 2.0.15 QNX port installed into the SDP
   (`$QNX_TARGET/usr/local/include/mosquitto_plugin.h`,
   `$QNX_TARGET/usr/local/include/mosquitto_broker.h`,
   `$QNX_TARGET/usr/local/lib/libmosquitto.*`) — **NOT installed yet**.
   Until it is, the plugin builds against local verified copies of the
   upstream headers in `plugin/build/host-include/` (byte-identical to
   qnx-ports/mosquitto v2.0.15; see `third-party/mosquitto/`) and links WITHOUT
   `-lmosquitto` — the broker resolves plugin symbols at load time.
   The plugin is built ON TOP of that port; it does not re-port
   Mosquitto.

## Build (x86_64, the first target)

From `gateway/`:

```sh
source ~/qnx/qnx800/qnxsdp-env.sh
make -C plugin -f Makefile.qnx clean
make -C plugin -f Makefile.qnx
```

Expected output: `plugin/qnx_bridge.so` (a shared library exporting
`mosquitto_plugin_version`, `mosquitto_plugin_init`,
`mosquitto_plugin_cleanup`). Verify the exports before going further:

```sh
ntox86_64-nm -D plugin/qnx_bridge.so | grep mosquitto_plugin_
# must list: mosquitto_plugin_version, mosquitto_plugin_init,
#            mosquitto_plugin_cleanup
```

The exact qcc command the Makefile runs (for reference / debugging):

```sh
qcc -Vgcc_ntox86_64 -shared -fPIC -O2 -Wall -Wextra -std=c11 \
  -I"$QNX_TARGET/usr/local/include" -I../common \
  -c qnx_bridge.c -o qnx_bridge.o
qcc -Vgcc_ntox86_64 -shared -fPIC -O2 -Wall -Wextra -std=c11 \
  -I"$QNX_TARGET/usr/local/include" -I../common \
  -c qnx_ipc_qnx.c -o qnx_ipc_qnx.o
qcc -Vgcc_ntox86_64 -shared -fPIC -O2 -Wall -Wextra -std=c11 \
  -I"$QNX_TARGET/usr/local/include" -I../common \
  -c ../common/qnx_msg.c -o ../common/qnx_msg.o
qcc -Vgcc_ntox86_64 -shared -L"$QNX_TARGET/usr/local/lib" -lmosquitto \
  -o qnx_bridge.so qnx_bridge.o qnx_ipc_qnx.o ../common/qnx_msg.o
```

**Notes / known risks for the first qcc run** (the QNX backend file has
never been compiled — read before debugging):

- `plugin/qnx_ipc_qnx.c` is guarded by `#ifdef __QNX__` and `#error`s on
  the host. It uses `name_attach(NULL, name, 0)`, `ConnectAttach`,
  `TimerTimeout(CLOCK_REALTIME, _NTO_TIMEOUT_RECEIVE, ...)` with a
  `SIGEV_PULSE_INIT` event, `MsgReceive` + `_msg_info.msglen`,
  `MsgReply`, `MsgSendPulse`. If any of these names/signatures differ in
  SDP 8.0, fix this file (and only this file) — the plugin and framing
  code are backend-independent.
- `MsgSend` returns the status the peer passed to `MsgReply()`; the
  backend treats non-zero as an error and derives the reply length from
  the framed ack's own header (see the comment in `qnx_ipc_send`).
- If `QNX_TARGET` is unset the Makefile fails immediately with a clear
  error instead of invoking qcc with empty include paths.

## aarch64 (stretch goal, untested)

```sh
source ~/qnx/qnx800/qnxsdp-env.sh
make -C plugin -f Makefile.qnx TARGET=gcc_ntoaarch64le
```

## What is already verified (host only)

- `test/run_tests.sh` builds the plugin + unit tests with host gcc and
  runs them to PASS (log: `test/host-build.log`). That proves the plugin
  logic, framing, queues, registry, routing, and the host IPC stub —
  it does **not** prove the QNX backend file compiles or that the `.so`
  loads in the QNX broker. The first real proof is the `nm -D` check
  above, then the QEMU demo in `docs/TEST-PLAN.md` (agent B).

## 4. Fixes applied on the first qcc run (25 Nov 2026)

All verified by a clean rebuild via both Makefiles:

- **`TimerTimeout` signature** (`plugin/qnx_ipc_qnx.c`): SDP 8.0 takes
  `const _Uint64t *ntime` (nanoseconds), NOT `struct timespec *`.
  Fixed to pass `(uint64_t)timeout_ms * 1000000ULL`. The old code
  compiled on the host stub path but was a type error under qcc.
- **`-D_QNX_SOURCE`** added to `plugin/Makefile.qnx` CFLAGS: with
  strict `-std=c11`, QNX extensions (`SIGEV_PULSE_INIT`,
  `SIGEV_PULSE_PRIO_INHERIT`, `usleep`) are hidden without it. This
  matches the Mosquitto port's own `common.mk`.
- **No `math.h` / `libm`** (`demo/qnx_sensor.c`): the SDP target tree
  has neither, so the sine drift is now a Taylor approximation
  (`demo_sin`, error ~1e-7); `-lm` dropped from `demo/Makefile.qnx`.
- **No `-lsocket` / `-lpthread`** (`demo/Makefile.qnx`): the target
  tree has no `libsocket.so`/`libpthread.so`; a link without them
  succeeds — all needed symbols resolve from `libc.so`.
- **Undefined-symbol sanity**: `nm -D --undefined-only` on both
  binaries shows only QNX IPC calls, libc/pthread symbols, and (plugin
  only) the four `mosquitto_broker_*` functions the broker provides at
  load time. No socket symbols anywhere.

## 5. Still blocked (25 Nov 2026, after the build)

- **Mosquitto broker: NOT built.** The port build needs the socket
  headers (`sys/socket.h`, `netinet/in.h`, `arpa/inet.h`, `netdb.h`)
  that the partial SDP install lacks. Until the SDP install gains the
  networking/libc-dev packages, no broker binary exists for the
  target — TEST-PLAN steps 3–6 cannot run. Do NOT work around this by
  hand-rolling socket headers.
- **QEMU boot: NOT attempted this run.** QEMU 8.2.2 was reinstalled
  (apt, after a VM restart wiped `/usr`), and the QSTI image is fully
  present (`~/qnx/quickstart/qemu/output/disk-qemu` 50.8 GB +
  `ifs.bin` 18.4 MB), but host RAM was only 613 MB available — the
  guest needs 1 GB and the standing rule is to boot only at ≥1500 MB.
  The hourly `qnx-boot-retry-when-quiet` cron owns the next attempt.

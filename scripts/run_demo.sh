#!/bin/sh
# run_demo.sh — host-side helper for the qnx-mqtt-gateway QEMU demo.
#
# Usage:   scripts/run_demo.sh <target-ip> [broker-port]
#          QNX_TARGET_HOST / MQTT_PORT env vars work too.
#
# What it does:
#   1. Checks prerequisites (mosquitto_pub, mosquitto_sub, ssh,
#      timeout) and that the target is reachable by SSH.
#   2. Subscribes to qnx/sensor/# on the target broker.
#   3. Sends the three demo commands (set_interval, led, ping).
#   4. Prints the telemetry + acks received.
#
# It is SAFE to run: it only checks first and exits with a clear
# message if anything is missing. It never installs, modifies, or
# restarts anything on the target — the broker and the demo app must
# already be running there (see docs/TEST-PLAN.md steps 3-4).
#
# Exit codes: 0 demo commands sent and telemetry seen,
#             1 prerequisite / connectivity problem (message explains),
#             2 commands sent but no telemetry/ack received.

TARGET="${1:-${QNX_TARGET_HOST:-}}"
PORT="${2:-${MQTT_PORT:-1883}}"

fail() {
    echo "run_demo.sh: ERROR: $*" >&2
    exit 1
}

[ -n "$TARGET" ] || fail "no target given — usage: $0 <target-ip> [broker-port] (or set QNX_TARGET_HOST)"
case "$PORT" in
    ''|*[!0-9]*) fail "broker port must be a number, got '$PORT'" ;;
esac

# --- 1. prerequisite checks -------------------------------------------
for cmd in mosquitto_pub mosquitto_sub ssh timeout; do
    command -v "$cmd" >/dev/null 2>&1 || \
        fail "'$cmd' not found on this host — install it first (mosquitto clients / openssh-client / coreutils)"
done

echo "run_demo.sh: target $TARGET, broker port $PORT"
echo "run_demo.sh: checking SSH to $TARGET ..."
if ! ssh -o BatchMode=yes -o ConnectTimeout=5 -o StrictHostKeyChecking=accept-new \
        "root@$TARGET" true 2>/dev/null; then
    fail "cannot SSH to root@$TARGET (key auth, 5 s timeout) — is the QEMU target up and sshd running? See docs/TEST-PLAN.md step 1-2."
fi
echo "run_demo.sh: SSH ok."

echo "run_demo.sh: checking broker answers on $TARGET:$PORT ..."
if ! timeout 5 mosquitto_sub -h "$TARGET" -p "$PORT" -t 'qnx/sensor/#' -C 1 >/dev/null 2>&1; then
    # -C 1 with no message in 5 s also exits 124 via timeout; that still
    # proves TCP connects. Distinguish "no route" from "no message yet"
    # with a plain connect probe instead:
    if ! timeout 5 mosquitto_pub -h "$TARGET" -p "$PORT" -t 'qnx/demo/probe' -m '' 2>/dev/null; then
        fail "cannot reach the broker at $TARGET:$PORT — is mosquitto running on the target? See docs/TEST-PLAN.md step 3."
    fi
    echo "run_demo.sh: broker reachable (no telemetry yet — is qnx_sensor running? See TEST-PLAN step 4)."
else
    echo "run_demo.sh: broker reachable, telemetry flowing."
fi

# --- 2. subscribe in the background, send commands ----------------------
OUT="$(mktemp)"
trap 'kill "$SUB_PID" 2>/dev/null; rm -f "$OUT"' EXIT INT TERM

echo "run_demo.sh: subscribing to qnx/sensor/# ..."
timeout 30 mosquitto_sub -h "$TARGET" -p "$PORT" -t 'qnx/sensor/#' -v >"$OUT" 2>&1 &
SUB_PID=$!
sleep 2   # let the subscription establish before publishing

echo "run_demo.sh: sending set_interval 5 ..."
mosquitto_pub -h "$TARGET" -p "$PORT" -t qnx/cmd/sensor/set_interval -m 5 || \
    fail "mosquitto_pub failed for set_interval"
sleep 2
echo "run_demo.sh: sending led on ..."
mosquitto_pub -h "$TARGET" -p "$PORT" -t qnx/cmd/sensor/led -m on || \
    fail "mosquitto_pub failed for led"
sleep 2
echo "run_demo.sh: sending ping ..."
mosquitto_pub -h "$TARGET" -p "$PORT" -t qnx/cmd/sensor/ping -m '' || \
    fail "mosquitto_pub failed for ping"

echo "run_demo.sh: waiting for telemetry + acks (up to ~20 s) ..."
sleep 12
kill "$SUB_PID" 2>/dev/null
wait "$SUB_PID" 2>/dev/null
trap - EXIT INT TERM

echo "--- received on qnx/sensor/# ---"
cat "$OUT"
rm -f "$OUT"

# --- 3. verdict -----------------------------------------------------------
echo "---"
echo "run_demo.sh: expected to see above:"
echo "  qnx/sensor/temp {\"temp_c\":..,\"unit\":\"C\"}   (every 5 s after set_interval)"
echo "  qnx/sensor/ack set_interval 5 ok"
echo "  qnx/sensor/ack led on ok"
echo "  qnx/sensor/status {\"status\":\"pong\",\"led\":\"on\"}"
echo "run_demo.sh: done. Restore the 2 s cadence with:"
echo "  mosquitto_pub -h $TARGET -p $PORT -t qnx/cmd/sensor/set_interval -m 2"
exit 0

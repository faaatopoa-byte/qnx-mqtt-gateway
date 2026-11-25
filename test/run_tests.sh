#!/bin/sh
# run_tests.sh — build the host binaries and run the gateway unit tests.
# Exits non-zero if the build or any test fails. Safe to re-run; stale
# stub sockets under /tmp are unlinked by the test binary itself.
#
# Usage: ./run_tests.sh            (from test/ or from gateway/)
#        ./run_tests.sh --verbose  (also show plugin broker logs)

set -e

cd "$(dirname "$0")/.."   # gateway/ — works from test/ or gateway/

if [ "$1" = "--verbose" ]; then
    export QNX_TEST_VERBOSE=1
fi

echo "=== gateway host build (gcc) ==="
make -C plugin -f Makefile.host clean
make -C plugin -f Makefile.host all

echo "=== unit tests ==="
./plugin/build/host/test_gateway
rc=$?

if [ $rc -eq 0 ]; then
    echo "=== ALL HOST TESTS PASSED ==="
else
    echo "=== HOST TESTS FAILED (exit $rc) ===" >&2
    exit $rc
fi

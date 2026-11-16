#!/bin/sh
#
# End to end test suite for vncsixel.
#
#   ./tests/run_e2e.sh
#
# Requires: python3 (with PIL and numpy for the image comparison),
#           libsixel's sixel2png, and the built binaries.

set -e

cd "$(dirname "$0")/.."

PYTHON=${PYTHON:-python3}
SIXEL2PNG=${SIXEL2PNG:-/tmp/opencode/src/libsixel-master/converters/sixel2png}

if [ ! -x ./vnt ] || [ ! -x ./vnctestserver ]; then
    echo "build the binaries first: make"
    exit 1
fi

if [ ! -x "$SIXEL2PNG" ]; then
    echo "sixel2png not found at $SIXEL2PNG (set SIXEL2PNG=...)"
    exit 1
fi

status=0

run() {
    name=$1
    shift
    echo
    echo "=== $name ==="
    if "$@"; then
        :
    else
        echo "*** $name FAILED ***"
        status=1
    fi
}

run "pipeline (vnc -> rgb -> scale -> sixel -> png)" \
    $PYTHON tests/verify_pipeline.py

run "terminal control sequences" \
    $PYTHON tests/verify_terminal.py

run "live updates and input forwarding" \
    $PYTHON tests/verify_interactive.py

run "sixel capability detection" \
    $PYTHON tests/verify_detect.py

run "terminal resize handling" \
    $PYTHON tests/verify_resize.py

echo
if [ $status -eq 0 ]; then
    echo "ALL TESTS PASSED"
else
    echo "SOME TESTS FAILED"
fi
exit $status

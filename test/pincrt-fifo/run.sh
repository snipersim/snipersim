#!/bin/sh
# Runs both checks natively (glibc) and under Pin (PinCRT), each with a
# timeout, and prints a verdict per runtime.  Usage: ./run.sh [PIN_ROOT]
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
PIN_ROOT=${1:-$HERE/../../sde_kit/pinkit}
LIMIT=${TIMEOUT:-8}
cd "$HERE"
make -s PIN_ROOT="$PIN_ROOT" obj-intel64/fifo_check.so obj-intel64/fifo_app.exe native >/dev/null || { echo "build failed"; exit 2; }
# DynamoRIO twin, when DR_ROOT points at a DynamoRIO release
DR=""
if [ -n "${DR_ROOT:-}" ] && [ -x "$DR_ROOT/bin64/drrun" ]; then
   make -s DR_ROOT="$DR_ROOT" fifo_check_dr.so >/dev/null && DR="$DR_ROOT/bin64/drrun" || echo "(DynamoRIO client did not build)"
fi

run_one() {   # $1 label, rest: command
   label=$1; shift
   rm -rf "$FIFO_DIR"; mkdir -p "$FIFO_DIR"; mkfifo "$FIFO_DIR/noreader.fifo"
   # -k: a SIGTERM to a Pin-run process is queued for delivery to the
   # application, which never happens while it is stuck inside the tool's
   # runtime, so escalate to SIGKILL (which Pin cannot intercept).  Output
   # goes through a file, not a pipe: drrun's application outlives drrun
   # when a client thread is stuck, and an orphan holding a pipe would
   # block this script forever.
   timeout -k 2 "$LIMIT" "$@" > "$FIFO_DIR/out.txt" 2>&1; rc=$?
   out=$(cat "$FIFO_DIR/out.txt")
   res=$(printf '%s\n' "$out" | grep '^RESULT' | head -1)
   if [ $rc -eq 124 ] || [ $rc -eq 137 ]; then verdict="HANG (killed after ${LIMIT}s)"; else verdict="returned (rc=$rc)"; fi
   printf '%-22s %-28s %s\n' "$label" "$verdict" "${res:-<no RESULT line>}"
   # A hung Pin leaves its processes behind; only they name this directory.
   case "$FIFO_DIR" in /tmp/fifo-check.??????) pkill -9 -f "$FIFO_DIR" 2>/dev/null ;; esac
   [ -n "${VERBOSE:-}" ] && printf '%s\n' "$out" | sed 's/^/    | /'
}

export FIFO_DIR=$(mktemp -d /tmp/fifo-check.XXXXXX)
echo "check 1: fopen() of a regular file while another thread is blocked in fopen() of a FIFO"
run_one "glibc  (native)" ./fifo_check_native 1 "$FIFO_DIR"
run_one "PinCRT (pin tool)" "$PIN_ROOT/pin" -t obj-intel64/fifo_check.so -check 1 -- obj-intel64/fifo_app.exe "$FIFO_DIR"
echo "  control: the same, but the blocker sits in a raw open(2) instead of fopen()"
RAW_BLOCKER=1 run_one "PinCRT raw-open ctl" "$PIN_ROOT/pin" -t obj-intel64/fifo_check.so -check 1 -- obj-intel64/fifo_app.exe "$FIFO_DIR"
RAW_BLOCKER=1 run_one "glibc  raw-open ctl" ./fifo_check_native 1 "$FIFO_DIR"
# DynamoRIO: the two bodies on client threads (dr_create_client_thread) put
# only the client's private libc in play; the app-thread shape (drwrap
# callbacks, like the Pin tool) is listed too, see README.
[ -n "$DR" ] && APP_LINGER=1 run_one "DynamoRIO client thr" "$DR" -c fifo_check_dr.so 3 -- obj-intel64/fifo_app.exe "$FIFO_DIR"
[ -n "$DR" ] && run_one "DynamoRIO app thr" "$DR" -c fifo_check_dr.so 1 -- obj-intel64/fifo_app.exe "$FIFO_DIR"
echo "  control: blocker in a raw open(2), probe also a raw open(2) of a regular file"
RAW_BLOCKER=1 RAW_PROBE=1 run_one "PinCRT raw/raw ctl" "$PIN_ROOT/pin" -t obj-intel64/fifo_check.so -check 1 -- obj-intel64/fifo_app.exe "$FIFO_DIR"
RAW_BLOCKER=1 RAW_PROBE=1 run_one "glibc  raw/raw ctl" ./fifo_check_native 1 "$FIFO_DIR"
[ -n "$DR" ] && RAW_BLOCKER=1 RAW_PROBE=1 APP_LINGER=1 run_one "DynamoRIO client thr" "$DR" -c fifo_check_dr.so 3 -- obj-intel64/fifo_app.exe "$FIFO_DIR"
echo
echo "check 2: read() on an empty pipe after fcntl(F_SETFL, O_NONBLOCK)"
run_one "glibc  (native)" ./fifo_check_native 2 "$FIFO_DIR"
run_one "PinCRT (pin tool)" "$PIN_ROOT/pin" -t obj-intel64/fifo_check.so -check 2 -- obj-intel64/fifo_app.exe "$FIFO_DIR"
[ -n "$DR" ] && run_one "DynamoRIO (client)" "$DR" -c fifo_check_dr.so 2 -- obj-intel64/fifo_app.exe "$FIFO_DIR"
rm -rf "$FIFO_DIR"

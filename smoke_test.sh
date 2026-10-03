#!/bin/sh
# Gravitas smoke test — boots the ISO headless and exercises the console.
#
# Covers:
#   * boot reaches the userland shell (kernel banner, VFS self-test, font load)
#   * keyboard -> shell echo -> command execution (via the QEMU monitor)
#   * backspace erase (raw hex scancodes: QEMU silently drops `sendkey bspc`)
#   * bare-name program spawn + VFS read (`cat hello.txt`)
#
# Artifacts land in test_artifacts/ (serial log + screendump).
# Run via `ninja test`, or directly after `ninja iso`.
set -u

ART="test_artifacts"
LOG="$ART/serial.log"
SHOT="$ART/console"

fail() {
    echo "FAIL: $*"
    echo "      (serial log: $ART/serial.log)"
    exit 1
}

command -v qemu-system-x86_64 >/dev/null || fail "qemu-system-x86_64 not installed"
command -v python3 >/dev/null || fail "python3 not installed"
[ -f gravitas.iso ] || fail "gravitas.iso missing — run 'ninja iso' first"

mkdir -p "$ART"
rm -f "$LOG" "$SHOT.ppm" "$SHOT.png"
TMP="$(mktemp -d)"
SOCK="$TMP/qmon.sock"
QPID=""

cleanup() {
    [ -n "$QPID" ] && kill "$QPID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

ACCEL=""
[ -e /dev/kvm ] && ACCEL="-enable-kvm"

echo "== booting gravitas.iso (headless) =="
qemu-system-x86_64 -cdrom gravitas.iso -m 512M \
    -display none -no-reboot $ACCEL \
    -serial "file:$LOG" -monitor "unix:$SOCK,server,nowait" &
QPID=$!

# wait_for <pattern>: poll the serial log (60s per marker)
wait_for() {
    pat="$1"
    i=0
    while [ "$i" -lt 300 ]; do
        grep -qE "$pat" "$LOG" 2>/dev/null && return 0
        i=$((i + 1))
        sleep 0.2
    done
    fail "timeout waiting for: $pat"
}

wait_for 'GRAVITAS OS KERNEL BOOTED'
wait_for 'SELF-TEST OK'
wait_for "type 'help'"
echo "== boot markers ok =="

# The shipped state must load the PSF2 font, not fall back
if grep -q 'keeping 8x8' "$LOG"; then
    fail "font fallback triggered (expected PSF2 8x16)"
fi
grep -qE '\[font\] loaded .*8x16' "$LOG" || fail "PSF2 font did not load"
echo "== font loaded (8x16) =="

# --- Drive the shell through the QEMU monitor ---
# Printable keys by name (reliable); special keys as raw set-1 scancodes
# (make/break pairs) because `sendkey bspc` is dropped by QEMU.
echo "== driving the shell =="
python3 - "$SOCK" <<'PYEOF' || fail "monitor session error"
import socket, sys, time

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1])
time.sleep(0.5)
s.recv(65536)  # banner

def mon(cmd):
    s.sendall((cmd + '\n').encode())
    time.sleep(0.25)
    s.settimeout(0.4)
    try:
        s.recv(65536)
    except Exception:
        pass
    s.settimeout(5)

def key(name):
    mon('sendkey ' + name)

def raw(make, brk):
    mon('sendkey 0x' + make)
    mon('sendkey 0x' + brk)

def type_str(text):
    for ch in text:
        key('spc' if ch == ' ' else ch)

# 1) Backspace: 'helpq' + BS -> 'help' -> help text proves the erase
for ch in 'helpq':
    key(ch)
raw('0e', '8e')   # backspace
key('ret')
time.sleep(2)

# 2) cat: bare-name spawn + VFS path fallback + file read
type_str('cat hello')
raw('34', 'b4')   # '.'
type_str('txt')
key('ret')
time.sleep(2)

# 3) Fault contract: crash.elf faults -> task dies, OS survives
type_str('run crash')
key('ret')
time.sleep(3)

# 4) Stack protector: smash.elf overruns a buffer -> detected, exits
type_str('run smash')
key('ret')
time.sleep(3)

mon('screendump ' + 'test_artifacts/console.ppm')
s.close()
PYEOF

wait_for 'commands are programs' || true
if ! grep -q 'commands are programs' "$LOG"; then
    fail "backspace test: 'help' output missing (erase or input broken?)"
fi
echo "== backspace erase ok =="

if ! grep -q 'Hello from the Gravitas tar' "$LOG"; then
    fail "cat hello.txt output missing (spawn/VFS/read broken?)"
fi
echo "== cat hello.txt ok =="

# Fault contract: crash.elf must fault loudly and die cleanly — and the
# shell must come back afterward (assert on output AFTER the kill line)
if ! grep -q "FAULT: USERLAND in task 'crash.elf'" "$LOG"; then
    fail "crash.elf fault dump missing (fault contract broken?)"
fi
if ! grep -q 'terminating task per the fault contract' "$LOG"; then
    fail "crash.elf was not terminated cleanly"
fi
sed -n '/terminating task per the fault contract/,$p' "$LOG" \
    | grep -q 'program finished' \
    || fail "shell did not survive the crash (spawn-wait never returned)"
echo "== fault contract ok (crash killed, shell survived) =="

# Stack protector: smash.elf must be caught by the canary
if ! grep -q 'stack smashing detected' "$LOG"; then
    fail "stack smash not detected (SSP wiring broken?)"
fi
sed -n '/stack smashing detected/,$p' "$LOG" \
    | grep -q 'program finished' \
    || fail "shell did not survive the stack smash"
echo "== stack protector ok (smash detected, shell survived) =="

# Human-readable screenshot (best effort)
if command -v ffmpeg >/dev/null; then
    ffmpeg -loglevel error -i "$SHOT.ppm" -y "$SHOT.png" 2>/dev/null || true
fi

echo ""
echo "SMOKE TEST PASS — artifacts in $ART/"
exit 0

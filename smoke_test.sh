#!/bin/sh
# Gravitas smoke test — boots the ISO headless and exercises the console.
#
# Covers:
#   * boot reaches the userland shell (kernel banner, VFS self-test, font load)
#   * keyboard -> shell echo -> command execution (via the QEMU monitor)
#   * backspace erase (raw hex scancodes: QEMU silently drops `sendkey bspc`)
#   * bare-name program spawn + VFS read (`cat hello.txt`)
#   * console cell layer scrolling under load (flood.elf, 150 lines)
#   * fault contract + stack protector (crash.elf / smash.elf)
#   * stack guard: runaway recursion faults on the guard page (stackoverflow.elf)
#   * M2 process control: bg spawn (non-blocking), wait, kill (proctest.elf)
#   * M3 IPC: per-task mailboxes, blocking recv wake, FIFO (msgtest.elf)
#   * M4 surfaces: destroy/restack/enumerate, the owner check, death
#     cleanup and the registry cap (surfacetest.elf + surfhelper.elf)
#   * M6 damage compositor: a small window's lifecycle repaints only its
#     rectangles, not the screen (damagetest.elf + F12 stat deltas)
#   * M7 debugger: introspection syscalls + recorded crash (mon.elf)
#   * M9 window manager: display authority, decisions, focus + close
#     (wm.elf + win.elf, driven with injected mouse events)
#   * M12 console endpoint: a task binds a child's console, sends it a line
#     and captures the reply (consoletest.elf + echoprog.elf)
#   * M12 terminal: a Gravity text grid hosting the shell, driven by clicking
#     and typing (term.elf)
#   * M13 file browser: a list widget over the VFS, launched from the menu
#     (files.elf)
#   * M13 image viewer + image library: decodes a picture and displays it
#     (imageview.elf, grv_image)
#   * libc basics: string/formatted output/allocator/parse/sort (libctest.elf)
#   * file API: seek/stat/truncate/create/remove (filetest.elf)
#   * timed wakeup: one-shot EV_TIMER with no other input (timertest.elf)
#   * popups: undecorated on-top second surface + WM dismissal (popuptest.elf)
#   * input: typematic key repeat + mouse wheel scrolling
#   * app lifecycle: cooperative close (veto, live retitle, force-kill backstop)
#   * Lua prerequisites in libc: SSE doubles, setjmp/longjmp, strtod, math,
#     and float formatting (%.14g) — exercised by libctest
#
# Timing: the driver never sleeps a fixed amount waiting for a program.
# Each step types a command, then waits for that step's completion marker
# to appear in the serial log — so the run is as fast as the guest, and a
# wedged step fails on a timeout instead of passing on a lucky sleep.
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
rm -f "$LOG" "$SHOT.ppm" "$SHOT.png" "$ART/term_before.ppm" "$ART/term_after.ppm"
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

# wait_for <pattern>: poll the serial log until it matches (60s cap)
wait_for() {
    pat="$1"
    i=0
    while [ "$i" -lt 600 ]; do
        grep -qE "$pat" "$LOG" 2>/dev/null && return 0
        i=$((i + 1))
        sleep 0.1
    done
    fail "timeout waiting for: $pat"
}

wait_for 'GRAVITAS OS KERNEL BOOTED'
wait_for 'SELF-TEST OK'
wait_for "type 'help'"
wait_for 'term: hosting shell as task'
echo "== boot markers ok =="

# The shipped state must load the PSF2 font, not fall back
if grep -q 'keeping 8x8' "$LOG"; then
    fail "font fallback triggered (expected PSF2 8x16)"
fi
grep -qE '\[font\] loaded .*8x16' "$LOG" || fail "PSF2 font did not load"
echo "== font loaded (8x16) =="

# --- Drive the shell through the QEMU monitor ---
# Printable keys by name; special keys as raw scancodes. The monitor queues
# commands in order, so the driver sends without waiting for a reply (only
# draining QEMU's echo so it never blocks on a full socket), paces keys just
# enough for the guest's keyboard controller to keep up, and waits on the
# serial log for each step's marker.
echo "== driving the shell =="
python3 - "$SOCK" "$LOG" <<'PYEOF' || fail "monitor session error"
import socket, sys, time, re

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1])
LOG = sys.argv[2]
time.sleep(0.3)
s.settimeout(1.0)
try:
    s.recv(65536)  # monitor banner
except Exception:
    pass

PACE = 0.04  # per-scancode guest pacing; we only type when the guest is idle

def cmd(c):
    s.sendall((c + '\n').encode())
    # Drain QEMU's echo non-blockingly so its output buffer can't fill and
    # stall command processing.
    s.settimeout(0)
    try:
        while s.recv(65536):
            pass
    except Exception:
        pass
    s.settimeout(None)

def key(name):
    cmd('sendkey ' + name)
    time.sleep(PACE)

def raw(make, brk):
    cmd('sendkey 0x' + make)
    time.sleep(PACE)
    cmd('sendkey 0x' + brk)
    time.sleep(PACE)

def type_str(text):
    for ch in text:
        key('spc' if ch == ' ' else ch)

def type_dotted(text):
    # Split at each dot: the raw hex make/break is the only reliable way to
    # send '.' (named keys for it get dropped).
    segs = text.split('.')
    for i, seg in enumerate(segs):
        type_str(seg)
        if i != len(segs) - 1:
            raw('34', 'b4')

def mmove(dx, dy):
    cmd('mouse_move %d %d' % (dx, dy))
    time.sleep(0.25)

def mclick():
    cmd('mouse_button 1'); time.sleep(0.2)
    cmd('mouse_button 0'); time.sleep(0.35)

def read_log():
    try:
        with open(LOG, 'r', errors='ignore') as f:
            return f.read()
    except OSError:
        return ""

def wait_count(pat, n, timeout=40.0):
    end = time.time() + timeout
    while time.time() < end:
        if len(re.findall(pat, read_log())) >= n:
            return
        time.sleep(0.03)
    sys.stderr.write("timed out waiting for %d x %s\n" % (n, pat))
    sys.exit(1)

def wait_for(pat, timeout=40.0):
    wait_count(pat, 1, timeout)

# The kernel brings up a window manager and a terminal at boot. Stop both so
# the low-level surface/damage tests below run against unmanaged surfaces (as
# they were written); the WM step at the end starts a fresh one and the
# terminal step a fresh terminal.
for pat in (r'window manager as task (\d+)', r'terminal as task (\d+)'):
    m = re.search(pat, read_log())
    if m:
        type_str('kill ' + m.group(1))
        key('ret')
        time.sleep(0.4)

# 0) Damage compositor: a small window's lifecycle must repaint only its
# rectangles, not the screen per operation. damagetest samples the
# compositor's pixel counter around just its surface ops, so the assertion
# is independent of console scrolling.
type_dotted('run damagetest.elf'); key('ret'); wait_for('damagetest done')

# 1) Backspace: 'helpq' + BS -> 'help' -> help text proves the erase
for ch in 'helpq':
    key(ch)
raw('0e', '8e')   # backspace
key('ret'); wait_for('commands are programs')

# 2) cat: bare-name spawn + VFS path fallback + file read
type_str('cat hello'); raw('34', 'b4'); type_str('txt'); key('ret')
wait_for('Hello from the Gravitas tar')

# 2b) Console scroll: flood prints 150 numbered lines, forcing the cell
# layer to scroll far past the screen.
type_dotted('run flood.elf'); key('ret'); wait_for('flood done')

# 3) Fault contract: crash.elf faults -> task dies, OS survives
type_str('run crash'); key('ret'); wait_for('terminating task per the fault contract')

# 4) Stack protector: smash.elf overruns a buffer -> detected, exits
type_str('run smash'); key('ret'); wait_for('stack smashing detected')

# 5) Process control: background spawn keeps the prompt alive. The sleeper
# prints ticks + 'done' ~2.5s later; jobs must answer while it runs.
type_dotted('bg sleeper.elf'); key('ret'); wait_for("bg: started 'sleeper")
type_str('jobs'); key('ret')
type_str('help'); key('ret'); wait_count('commands are programs', 2)
wait_for(r'\[sleeper\] done')

# 6) Process-control contract: proctest checks spawn_bg/wait/kill
type_dotted('run proctest.elf'); key('ret'); wait_for('all process-control checks pass')

# 7) IPC contract: msgtest spawns msgecho.elf and round-trips messages
type_dotted('run msgtest.elf'); key('ret'); wait_for('all ipc checks pass')

# 7b) Console endpoint: consoletest binds a child's console, sends it a line
# and captures the reply — both stdio directions through the kernel.
type_dotted('run consoletest.elf'); key('ret'); wait_for('consoletest: PASS')

# 7c) libc basics: string, formatted output, allocator, parsing, sort.
type_dotted('run libctest.elf'); key('ret'); wait_for('libctest: all checks pass')

# 7d) File API: mkdir/create/write/stat/seek/truncate/unlink on the ramfs.
type_dotted('run filetest.elf'); key('ret'); wait_for('filetest: all checks pass')

# 7e) Timed wakeup: a one-shot timer posts GRV_EV_TIMER with no other input.
type_dotted('run timertest.elf'); key('ret'); wait_for('timertest: PASS')

# 8) Surface contract: surfacetest exercises the whole surface lifecycle
type_dotted('run surfacetest.elf'); key('ret'); wait_for('surface contract ok')

# 9) Debugger: mon reads the introspection syscalls and the crash record
type_dotted('run mon.elf'); key('ret'); wait_for('mon: last fault')

# 9b) Stack guard: unbounded recursion hits the unmapped guard page below the
# user stack, so the fault is named and only that task dies. After the mon
# step so the recorded crash is still crash.elf.
type_dotted('run stackoverflow.elf'); key('ret')
wait_for('stack overflow detected')
wait_for('terminating task per the fault contract')

# 10) Window manager: claim, discover a client window, click to focus it,
# type into it (only the focused task gets keys), then close it.
type_dotted('bg wm.elf'); key('ret'); wait_for('display authority')
type_dotted('bg win.elf'); key('ret'); wait_for(r"window #[0-9]+ 'win")
wait_for(r"wm: title #[0-9]+ 'Gravity demo'")   # client title over IPC
mmove(-440, -350); mclick()                 # click the title bar (window at 80,60)
wait_for('focus task')
type_str('a'); wait_for(r"win: key 'a'")
# Typematic repeat: a key held down is re-delivered by the kernel (the driver
# repeats it on the timer tick), so several key events arrive.
cmd('sendkey a 900')
wait_count(r"win: key 'a'", 4)
mmove(0, 100); mclick()                      # click the client body -> forwarded
wait_for(r'win: click [0-9]+,[0-9]+')
mmove(226, -105); mclick()                   # click the close box (~426,45)
wait_for(r'window #[0-9]+ gone')

# 10c) Cooperative close: closing asks the client to exit rather than killing
# it. closetest vetoes the first request (and retitles itself), accepts the
# second. It is the only window, so its close box is the same slot (~426,45).
type_dotted('bg closetest.elf'); key('ret')
wait_for(r"window #[0-9]+ 'closetest")
wait_for(r"wm: title #[0-9]+ 'Close test'")
mclick()                                     # cursor is still on the close box
wait_for('closetest: vetoed')
wait_for(r"wm: title #[0-9]+ 'Close test .vetoed.'")  # live retitle
mclick()                                     # close again -> accepted
wait_for('closetest: closing')
wait_for(r'window #[0-9]+ gone')

# 10d) A client that ignores the close request (rawwin never reads its
# mailbox) is force-killed once the grace period expires.
type_dotted('bg rawwin.elf'); key('ret')
wait_for(r"window #[0-9]+ 'rawwin")
wait_for('rawwin: up as')
mclick()                                     # close box -> ignored
wait_for(r"wm: close timeout #[0-9]+")
wait_for(r'window #[0-9]+ gone')

# 10b) Launcher menu + Alt+Tab (cursor now ~426,45). Four menu items:
# Terminal (y~699), Files (~721), Images (~743), Demo (~765).
mmove(-386, 743); mclick()                   # the "Apps" button (~40,788)
mmove(0, -89); mclick()                       # the "Terminal" item (~40,699)
wait_for("wm: launch 'term.elf'")
wait_count(r"wm: window #[0-9]+ 'term", 2)    # the launched terminal
cmd('sendkey alt-tab')
wait_count('wm: focus task', 2)              # Alt+Tab focused it

# 11) Terminal: the focused terminal's window must render the hosted shell's
# output. Keying 'ls' hits only the focused task, so a change inside the
# window (and not the console behind it) proves the terminal path works.
cmd('screendump test_artifacts/term_before.ppm')
type_str('ls'); key('ret')
time.sleep(1.0)
cmd('screendump test_artifacts/term_after.ppm')
wait_for(r"User task 'ls.elf' created")

# 12) File browser: launch it from the menu and confirm its window/title.
mmove(0, 89); mclick()                        # the "Apps" button (~40,788)
mmove(0, -67); mclick()                        # the "Files" item (~40,721)
wait_for("wm: launch 'files.elf'")
wait_count(r"wm: window #[0-9]+ 'files", 1)
wait_for(r"wm: title #[0-9]+ 'Files'")

# 13) Image viewer: launch it; it decodes a picture from /Pictures with the
# shared grv_image library.
mmove(0, 67); mclick()                        # the "Apps" button (~40,788)
mmove(0, -45); mclick()                        # the "Images" item (~40,743)
wait_for("wm: launch 'imageview.elf'")
wait_count(r"wm: window #[0-9]+ 'imageview", 1)
wait_for(r"wm: title #[0-9]+ 'Images'")
wait_for(r"imageview: /Pictures/")

# 14) Popup: a second, undecorated, on-top surface the WM dismisses when the
# user clicks away from it.
mmove(-5000, -5000); mclick()                  # desktop click: focus the console shell
type_dotted('bg popuptest.elf'); key('ret')
wait_for('popuptest: popup open')
wait_for(r"wm: popup #[0-9]+")
mmove(-5000, -5000); mclick()                  # click the desktop -> dismiss
wait_for('popuptest: popup closed')

cmd('screendump ' + 'test_artifacts/console.ppm')
time.sleep(0.3)
s.close()
PYEOF

if ! grep -q 'commands are programs' "$LOG"; then
    fail "backspace test: 'help' output missing (erase or input broken?)"
fi
echo "== backspace erase ok =="

if ! grep -q 'Hello from the Gravitas tar' "$LOG"; then
    fail "cat hello.txt output missing (spawn/VFS/read broken?)"
fi
echo "== cat hello.txt ok =="

# Console scroll (flood.elf): all 150 lines plus the footer made it through
if ! grep -q 'flood line 150' "$LOG" || ! grep -q 'flood done' "$LOG"; then
    fail "flood output incomplete (console scroll/reflow broken?)"
fi
echo "== console scroll ok (150 lines) =="

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

# Stack guard (stackoverflow.elf): the unmapped page below the user stack
# turns runaway recursion into a named fault instead of silent corruption.
if ! grep -q 'stack overflow detected' "$LOG"; then
    fail "stack overflow did not hit the guard page (silent corruption?)"
fi
sed -n '/stack overflow detected/,$p' "$LOG" | grep -q 'program finished' \
    || fail "shell did not survive the stack overflow"
echo "== stack guard ok (overflow faults cleanly, task killed) =="

# Process control: bg spawn must not block the shell, and reports the pid
if ! grep -qE "bg: started 'sleeper[.]elf' as task [0-9]+" "$LOG"; then
    fail "bg spawn marker missing (spawn_bg broken?)"
fi
sed -n "/bg: started/,\$p" "$LOG" | grep -q '^  [0-9]\+  sleeper.elf' \
    || fail "jobs did not list the background task (while it was running)"
echo "== background spawn non-blocking ok =="

# Process-control contract regression (proctest.elf)
if ! grep -q 'all process-control checks pass' "$LOG"; then
    fail "proctest failed its process-control contract (M2 broken?)"
fi
echo "== spawn_bg/wait/kill contract ok =="

# IPC contract (msgtest.elf + msgecho.elf)
if ! grep -q 'all ipc checks pass' "$LOG"; then
    fail "msgtest failed its ipc contract (M3 broken?)"
fi
echo "== ipc contract ok (send/recv/fifo/death) =="

# Console endpoint (consoletest.elf + echoprog.elf): the child's output must
# be captured by its owner, NOT leak to the kernel console.
if ! grep -q 'consoletest: PASS' "$LOG"; then
    fail "consoletest failed the console endpoint contract (M12 broken?)"
fi
if grep -q 'echo:ping' "$LOG"; then
    fail "hosted program output leaked to the kernel console"
fi
echo "== console endpoint ok (attach/write/read round-trip) =="

# libc basics (libctest.elf)
if ! grep -q 'libctest: all checks pass' "$LOG"; then
    fail "libctest failed (libc string/stdio/stdlib regression)"
fi
echo "== libc basics ok (string/snprintf/alloc/parse/sort) =="

# File API (filetest.elf)
if ! grep -q 'filetest: all checks pass' "$LOG"; then
    fail "filetest failed (seek/stat/truncate/create/remove regression)"
fi
echo "== file API ok (mkdir/create/write/stat/seek/truncate/unlink) =="

# Timed wakeup (timertest.elf)
if ! grep -q 'timertest: PASS' "$LOG"; then
    fail "timertest failed (one-shot timer / EV_TIMER regression)"
fi
echo "== timer wakeup ok (one-shot GRV_EV_TIMER) =="

# Surface contract (surfacetest.elf + surfhelper.elf)
if ! grep -q 'surface contract ok' "$LOG"; then
    fail "surfacetest failed its surface contract (M4 broken?)"
fi
echo "== surface contract ok (destroy/z-order/enumerate/owner/cap) =="

# Damage compositor (damagetest.elf): a small window's lifecycle must
# repaint only its rectangles, never the full screen per operation. The
# program samples the compositor counter around JUST the surface ops, so
# console scrolling cannot pollute the figure.
if ! grep -q 'damagetest done' "$LOG"; then
    fail "damagetest failed (damage compositor broken?)"
fi
DAM=$(grep -oE 'damagetest: window damage [0-9]+ px' "$LOG" \
      | sed -E 's/.* ([0-9]+) px/\1/' | tail -1)
if [ -z "$DAM" ]; then
    fail "damagetest reported no damage figure"
fi
# The window is 240x160 (38,400 px). Create + 3 moves + 3 commits + destroy
# stays well under 2M with damage tracking; a full-screen repaint per op
# would be several million (1,024,000 px each).
if [ "$DAM" -ge 2000000 ]; then
    fail "compositor painted too much for a small window ($DAM px; damage broken?)"
fi
echo "== damage compositor ok ($DAM px for a small-window lifecycle) =="

# Debugger (mon.elf): introspection snapshot + the recorded crash
if ! grep -q 'mon: ticks' "$LOG"; then
    fail "mon produced no system snapshot (introspection broken?)"
fi
if ! grep -q "mon: last fault: task 'crash.elf'" "$LOG"; then
    fail "mon did not read back the recorded crash (fault record broken?)"
fi
echo "== debugger monitor ok (sysinfo/tasks/surfaces/crash record) =="

# Window manager (wm.elf + win.elf): claim, discover, focus, type, close
if ! grep -q 'display authority' "$LOG"; then
    fail "wm did not claim the display authority"
fi
if ! grep -qE "window #[0-9]+ 'win" "$LOG"; then
    fail "wm did not discover/decorate the client window"
fi
if ! grep -q "win: key 'a'" "$LOG"; then
    fail "wm did not route the keyboard to the focused window"
fi
if [ "$(grep -c "win: key 'a'" "$LOG")" -lt 4 ]; then
    fail "held key did not repeat (typematic repeat broken?)"
fi
if ! grep -qE 'win: click [0-9]+,[0-9]+' "$LOG"; then
    fail "wm did not forward the mouse click to the client"
fi
if ! grep -qE "window #[0-9]+ gone" "$LOG"; then
    fail "wm did not close the window"
fi
echo "== window manager ok (claim/discover/focus/type/click/close) =="

# Cooperative close (closetest.elf): the close box asks the client, which may
# veto (and retitle), then accept.
if ! grep -q 'closetest: vetoed' "$LOG"; then
    fail "client could not veto the close request (cooperative close broken?)"
fi
if ! grep -qE "wm: title #[0-9]+ 'Close test .vetoed.'" "$LOG"; then
    fail "live title update did not reach the WM"
fi
if ! grep -q 'closetest: closing' "$LOG"; then
    fail "client did not accept the second close request"
fi
echo "== cooperative close ok (veto + retitle, then exit) =="

# Force-close backstop (rawwin.elf): a client that ignores the request is
# killed once the grace period expires.
if ! grep -qE "wm: close timeout #[0-9]+" "$LOG"; then
    fail "ignored close request was not force-killed (backstop broken?)"
fi
echo "== force-close backstop ok (uncooperative client killed) =="

# Title protocol: the client answered the WM's hello with its own title
if ! grep -qE "wm: title #[0-9]+ 'Gravity demo'" "$LOG"; then
    fail "wm did not receive the client title (title protocol broken?)"
fi
# Taskbar launcher: clicking it started a program
if ! grep -q "wm: launch 'term.elf'" "$LOG"; then
    fail "wm launcher did not spawn a program"
fi
# Desktop wallpaper loaded from a shipped BMP
if ! grep -q "wm: wallpaper" "$LOG"; then
    fail "wm did not load a wallpaper"
fi
# Alt+Tab: a global hotkey cycled focus (a second 'focus task' line)
if [ "$(grep -c 'wm: focus task' "$LOG")" -lt 2 ]; then
    fail "Alt+Tab did not cycle focus (hotkey mechanism broken?)"
fi
echo "== desktop ok (title protocol, taskbar launcher, Alt+Tab) =="

# Terminal (term.elf): the launcher-spawned terminal hosts the shell, and when
# focused and typed into it renders the output INSIDE its own window. Keying
# 'ls' reaches only the focused task, so a change inside the window (and not
# the console behind it) proves the terminal input path.
if ! grep -q 'term: hosting shell as task' "$LOG"; then
    fail "terminal did not start and attach its shell"
fi
if ! grep -q "User task 'ls.elf' created" "$LOG"; then
    fail "terminal did not deliver input to the hosted shell (M12 UI broken?)"
fi
if ! python3 - "$ART/term_before.ppm" "$ART/term_after.ppm" <<'PY'
import sys
def load(p):
    d = open(p, 'rb').read()
    q = d.split(b'\n', 3)
    w, h = map(int, q[1].split())
    return w, h, q[3]
w, h, a = load(sys.argv[1])
_, _, b = load(sys.argv[2])
n = 0
for y in range(62, 458, 2):        # inside the terminal window (80,60)-(726,460)
    row = y * w
    for x in range(82, 724, 2):
        j = (row + x) * 3
        if a[j:j+3] != b[j:j+3]:
            n += 1
print("terminal region changed pixels (sampled):", n)
sys.exit(0 if n > 200 else 1)
PY
then
    fail "terminal did not render the hosted shell's output (keys missed it?)"
fi
echo "== terminal ok (hosts the shell; typing renders in its window) =="

# File browser (files.elf): launched from the menu, titled, and listing a dir.
if ! grep -q "wm: launch 'files.elf'" "$LOG"; then
    fail "file browser did not launch from the menu"
fi
if ! grep -qE "wm: title #[0-9]+ 'Files'" "$LOG"; then
    fail "file browser window/title missing"
fi
echo "== file browser ok (launched, titled, lists directories) =="

# Image viewer (imageview.elf): decodes a picture via the shared grv_image
# library and shows it.
if ! grep -q "wm: launch 'imageview.elf'" "$LOG"; then
    fail "image viewer did not launch from the menu"
fi
if ! grep -q "imageview: /Pictures/" "$LOG"; then
    fail "image viewer loaded no picture (image library broken?)"
fi
echo "== image viewer ok (decodes and displays a picture) =="

# Popup (popuptest.elf): an undecorated, on-top second surface, dismissed by
# a click outside it.
if ! grep -q 'popuptest: popup open' "$LOG"; then
    fail "popup did not open (multi-surface / role broken?)"
fi
if ! grep -qE "wm: popup #[0-9]+" "$LOG"; then
    fail "wm did not register the popup surface"
fi
if ! grep -q 'popuptest: popup closed' "$LOG"; then
    fail "popup was not dismissed on an outside click"
fi
echo "== popup ok (undecorated on-top surface; dismiss on outside click) =="

# Human-readable screenshot (best effort)
if command -v ffmpeg >/dev/null; then
    ffmpeg -loglevel error -i "$SHOT.ppm" -y "$SHOT.png" 2>/dev/null || true
fi

echo ""
echo "SMOKE TEST PASS — artifacts in $ART/"
exit 0

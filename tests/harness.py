"""Gravitas test harness.

Boots the ISO headless and drives it through the QEMU monitor. A *group* is a
Python module that registers tests with @test; running it boots one guest and
runs each test in order (see run_module).

Hard-won rules baked in here:
  * QEMU's sendkey drops uppercase letters and the named keys for '.' and '/',
    so those go out as raw make/break scancodes.
  * mouse_move is relative and the kernel clamps the pointer to
    (screen - cursor), so the harness tracks the pointer in absolute screen
    coordinates and moves in small steps (a large jump leaves a packet
    backlog that swallows the next move).
  * Waits are anchored on the serial log, never a fixed sleep: a wedged step
    fails on its timeout instead of passing on a lucky sleep.
"""
import os
import re
import json
import shutil
import socket
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCREEN_W, SCREEN_H = 1280, 800      # the guest's framebuffer
CURSOR = 1                          # the pointer hotspot reaches the last pixel
                                    # (the kernel clips the sprite at the edge)
TASKBAR_H = 24
MENU_W, MENU_ITEM_H = 150, 22       # WM launcher geometry (src/user/wm.cpp)


class TestFailure(Exception):
    pass


def fail(msg):
    raise TestFailure(msg)


def load_apps():
    """The launcher manifest in menu order: [(program, label), ...]."""
    apps = []
    try:
        with open(os.path.join(ROOT, "files", "apps")) as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                parts = line.split(None, 1)
                apps.append((parts[0], parts[1] if len(parts) > 1 else parts[0]))
    except OSError:
        pass
    return apps


APP_LIST = load_apps()


class Guest:
    # Per-scancode pacing. QEMU injects a key the moment sendkey is called (no
    # floor of its own — measured), so this is the only limit; the guest's PS/2
    # controller drops keys if they arrive faster than its IRQ handler drains
    # it. 15ms is ~2.5x faster than the old 40ms and still green across runs.
    PACE = 0.015

    def __init__(self, name):
        self.name = name
        self.art = os.path.join(ROOT, "test_artifacts", name)
        os.makedirs(self.art, exist_ok=True)
        self.log_path = os.path.join(self.art, "serial.log")
        if os.path.exists(self.log_path):
            os.remove(self.log_path)
        self.tmp = tempfile.mkdtemp(prefix="gravitas-")
        self.sock_path = os.path.join(self.tmp, "qmon.sock")
        self.qmp_path = os.path.join(self.tmp, "qmp.sock")
        self.qemu = None
        self.sock = None
        self.qmp = None
        # The kernel starts the pointer at the screen centre (src/kernel/dev/mouse.cpp).
        self.cursor = [SCREEN_W // 2, SCREEN_H // 2]
        self._boot()

    # --- lifecycle -----------------------------------------------------

    def _connect(self, path, timeout=10.0):
        end = time.time() + timeout
        while True:
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(path)
                return s
            except OSError:
                if time.time() > end:
                    fail("QEMU socket never appeared: %s" % path)
                time.sleep(0.05)

    def _qmp(self, obj):
        """Send one QMP command and drain its reply (best effort)."""
        try:
            self.qmp.sendall((json.dumps(obj) + "\n").encode())
            self.qmp.settimeout(1.0)
            try:
                self.qmp.recv(65536)
            except OSError:
                pass
        finally:
            self.qmp.settimeout(None)

    def _boot(self):
        iso = os.path.join(ROOT, "gravitas.iso")
        if not os.path.exists(iso):
            fail("gravitas.iso missing - run 'ninja iso' first")
        if shutil.which("qemu-system-x86_64") is None:
            fail("qemu-system-x86_64 not installed")
        accel = ["-enable-kvm"] if os.path.exists("/dev/kvm") else []
        self.qemu = subprocess.Popen([
            "qemu-system-x86_64", "-cdrom", iso, "-m", "512M",
            # q35 boots in ~3.4s against the default i440fx's ~7.3s: the older
            # machine's IDE path makes Limine's read of the (tens of MB) tar
            # module the slowest thing in the whole suite. Both machines are
            # exercised by hand, and the kernel is happy on either.
            "-machine", "q35",
            "-display", "none", "-no-reboot", *accel,
            "-serial", "file:" + self.log_path,
            "-monitor", "unix:%s,server,nowait" % self.sock_path,
            # QMP is needed for input the HMP cannot express (the wheel).
            "-qmp", "unix:%s,server,nowait" % self.qmp_path,
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        self.sock = self._connect(self.sock_path)
        self.sock.settimeout(1.0)
        try:
            self.sock.recv(65536)   # monitor banner
        except OSError:
            pass
        self.sock.settimeout(None)

        self.qmp = self._connect(self.qmp_path)
        self.qmp.settimeout(2.0)
        try:
            self.qmp.recv(65536)    # QMP greeting
        except OSError:
            pass
        self._qmp({"execute": "qmp_capabilities"})

        # Shared boot preconditions. A failure here is a boot failure, reported
        # the same way for every group.
        self.wait_for("GRAVITAS OS KERNEL BOOTED", 60)
        self.wait_for("SELF-TEST OK", 60)
        self.wait_for("type 'help'", 60)
        self.wait_for("term: hosting shell as task", 60)

    def close(self):
        try:
            if self.sock:
                self.sock.close()
        except OSError:
            pass
        try:
            if self.qmp:
                self.qmp.close()
        except OSError:
            pass
        if self.qemu:
            self.qemu.terminate()
            try:
                self.qemu.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.qemu.kill()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def fail(self, msg):
        raise TestFailure(msg)

    # --- serial log ----------------------------------------------------

    def log(self):
        try:
            with open(self.log_path, errors="ignore") as f:
                return f.read()
        except OSError:
            return ""

    def count(self, pat):
        # re.M so ^/$ anchor to log lines, not the whole log.
        return len(re.findall(pat, self.log(), re.M))

    def mark(self):
        return len(self.log())

    def since(self, mark):
        return self.log()[mark:]

    def after(self, marker):
        """The log from the last occurrence of marker to the end."""
        text = self.log()
        idx = text.rfind(marker)
        return text[idx:] if idx >= 0 else ""

    def last_int(self, pat):
        m = None
        for m in re.finditer(pat, self.log()):
            pass
        return int(m.group(1)) if m else None

    def wait_count(self, pat, n, timeout=45.0):
        end = time.time() + timeout
        while time.time() < end:
            if self.count(pat) >= n:
                return
            time.sleep(0.03)
        fail("timeout waiting for %d x /%s/" % (n, pat))

    def wait_for(self, pat, timeout=45.0):
        self.wait_count(pat, 1, timeout)

    def wait_after(self, marker, needle, timeout=45.0):
        """Wait until the log, from the last occurrence of marker on, holds needle."""
        end = time.time() + timeout
        while time.time() < end:
            if needle in self.after(marker):
                return
            time.sleep(0.03)
        fail("never saw %r after %r" % (needle, marker))

    def wait_more(self, pat, n, timeout=45.0):
        self.wait_count(pat, n, timeout)

    def expect(self, needle, where="log"):
        if needle not in self.log():
            fail("expected %r in the %s" % (needle, where))

    # --- monitor: keyboard ---------------------------------------------

    def cmd(self, c):
        try:
            self.sock.sendall((c + "\n").encode())
        except OSError as e:
            fail("monitor command failed (%s): %s" % (c, e))
        # Drain QEMU's echo non-blockingly so its output buffer can't fill and
        # stall command processing.
        self.sock.settimeout(0)
        try:
            while self.sock.recv(65536):
                pass
        except OSError:
            pass
        self.sock.settimeout(None)

    def key(self, name):
        self.cmd("sendkey " + name)
        time.sleep(self.PACE)

    def raw(self, make, brk):
        self.cmd("sendkey 0x" + make)
        time.sleep(self.PACE)
        self.cmd("sendkey 0x" + brk)
        time.sleep(self.PACE)

    # Named keys those two characters must go through raw scancodes.
    SPECIAL = {".": ("34", "b4"), "/": ("35", "b5")}

    # Punctuation QEMU names by key, not by character. Shifted symbols are
    # shift-<key>. (Uppercase letters are handled as shift-<letter>.)
    NAMED = {
        "-": "minus", "=": "equal", ",": "comma", ";": "semicolon",
        "'": "apostrophe", "[": "bracket_left", "]": "bracket_right",
        "\\": "backslash", "`": "grave_accent",
        "!": "shift-1", "@": "shift-2", "#": "shift-3", "$": "shift-4",
        "%": "shift-5", "^": "shift-6", "&": "shift-7", "*": "shift-8",
        "(": "shift-9", ")": "shift-0", "_": "shift-minus", "+": "shift-equal",
        ":": "shift-semicolon", '"': "shift-apostrophe",
        "{": "shift-bracket_left", "}": "shift-bracket_right",
        "|": "shift-backslash", "<": "shift-comma", ">": "shift-dot",
        "?": "shift-slash", "~": "shift-grave_accent",
    }

    def type(self, text):
        for ch in text:
            if ch in self.SPECIAL:
                self.raw(*self.SPECIAL[ch])
            elif ch == " ":
                self.key("spc")
            elif ch.isupper():
                self.key("shift-" + ch.lower())
            elif ch in self.NAMED:
                self.key(self.NAMED[ch])
            else:
                self.key(ch)

    def shell(self, line):
        """Type a line into the focused console shell and press Enter."""
        self.type(line)
        self.key("ret")

    def hold(self, name, ms):
        self.cmd("sendkey %s %d" % (name, ms))

    def ctrl(self, name):
        self.cmd("sendkey ctrl-" + name)
        time.sleep(self.PACE)

    def alt(self, name):
        self.cmd("sendkey alt-" + name)
        time.sleep(self.PACE)

    def chord(self, *keys):
        """A modifier chord, e.g. chord("ctrl", "shift", "c")."""
        self.cmd("sendkey " + "-".join(keys))
        time.sleep(self.PACE)

    # --- monitor: pointer ----------------------------------------------

    def move_to(self, x, y, step=200):
        lim_x = SCREEN_W - CURSOR
        lim_y = SCREEN_H - CURSOR
        x = max(0, min(lim_x, int(x)))
        y = max(0, min(lim_y, int(y)))
        while (self.cursor[0], self.cursor[1]) != (x, y):
            dx = max(-step, min(step, x - self.cursor[0]))
            dy = max(-step, min(step, y - self.cursor[1]))
            self.cmd("mouse_move %d %d" % (dx, dy))
            self.cursor[0] += dx
            self.cursor[1] += dy
            time.sleep(0.06)
        time.sleep(0.08)

    def click(self, x=None, y=None):
        if x is not None:
            self.move_to(x, y)
        self.cmd("mouse_button 1")
        time.sleep(0.2)
        self.cmd("mouse_button 0")
        time.sleep(0.35)

    def wheel(self, notches):
        """Inject wheel notches: positive = up/away.

        The HMP has no wheel command (its mouse_button is L/R/M only), so this
        goes through QMP's input-send-event."""
        button = "wheel-up" if notches > 0 else "wheel-down"
        for _ in range(abs(int(notches))):
            self._qmp({"execute": "input-send-event",
                       "arguments": {"events": [
                           {"type": "btn",
                            "data": {"down": True, "button": button}}]}})
            time.sleep(0.05)
        time.sleep(0.2)

    def desktop(self):
        """Click empty desktop: focuses the console shell (dismisses popups)."""
        self.click(5, 5)

    # --- monitor: screenshots ------------------------------------------

    def screendump(self, path):
        self.cmd("screendump " + path)
        time.sleep(0.4)

    # --- WM introspection ----------------------------------------------

    def windows(self):
        """The latest geometry for every window the WM has created.

        The WM logs a window with its task name at creation and its client's
        title once the hello is answered, so a window carries both: "task" (the
        creation name) and "title" (what it ended up showing)."""
        out = {}
        for m in re.finditer(
                r"wm: window #(\d+) '([^']*)' (\d+)x(\d+) at (-?\d+),(-?\d+)", self.log()):
            out[int(m.group(1))] = {
                "id": int(m.group(1)), "task": m.group(2), "title": m.group(2),
                "w": int(m.group(3)), "h": int(m.group(4)),
                "x": int(m.group(5)), "y": int(m.group(6)),
            }
        for m in re.finditer(r"wm: title #(\d+) '([^']*)'", self.log()):
            w = out.get(int(m.group(1)))
            if w:
                w["title"] = m.group(2)   # the last title the WM logged
        return list(out.values())

    def window(self, title_pat):
        hit = None
        for w in self.windows():
            if re.search(title_pat, w["title"]) or re.search(title_pat, w["task"]):
                hit = w
        return hit

    def await_window(self, title_pat, timeout=45.0):
        end = time.time() + timeout
        while time.time() < end:
            w = self.window(title_pat)
            if w:
                return w
            time.sleep(0.03)
        fail("no window matching %r" % title_pat)

    def wait_window_gone(self, title_pat, timeout=45.0):
        wid = None
        for m in re.finditer(r"wm: window #(\d+) '([^']*)'", self.log()):
            if re.search(title_pat, m.group(2)):
                wid = m.group(1)
        if wid is None:
            fail("no window matching %r to wait for" % title_pat)
        self.wait_for(r"window #%s gone" % wid, timeout)
        time.sleep(0.3)

    def click_title(self, w):
        self.click(w["x"] + w["w"] // 2, w["y"] - 9)

    def click_body(self, w, dx=0, dy=0):
        self.click(w["x"] + w["w"] // 2 + dx, w["y"] + w["h"] // 2 + dy)

    def click_close(self, w):
        # close_rect: 12x12 at (x + w - 14, y - 15); its centre is (x+w-8, y-9)
        self.click(w["x"] + w["w"] - 8, w["y"] - 9)

    # --- launcher geometry ---------------------------------------------

    def apps_button(self):
        """The taskbar "Apps" button (clamped to the reachable pointer area)."""
        return (28, SCREEN_H - 12)

    def menu_item(self, index):
        n = len(APP_LIST)
        y = (SCREEN_H - TASKBAR_H) - n * MENU_ITEM_H + index * MENU_ITEM_H + 11
        return (4 + MENU_W // 2, y)

    def menu_rect(self):
        n = len(APP_LIST)
        y = (SCREEN_H - TASKBAR_H) - n * MENU_ITEM_H
        return (4, y, 4 + MENU_W, y + n * MENU_ITEM_H)

    def menu_index(self, label):
        for i, (_prog, lab) in enumerate(APP_LIST):
            if lab == label:
                return i
        fail("no launcher app labelled %r (files/apps)" % label)

    def launch(self, label):
        """Open the Apps menu and click the app with that label."""
        self.click(*self.apps_button())
        self.click(*self.menu_item(self.menu_index(label)))

    # --- crash artifacts -----------------------------------------------

    def dump(self, tag):
        try:
            self.screendump(os.path.join(self.art, tag + ".ppm"))
        except TestFailure:
            pass


# --- screenshots -------------------------------------------------------

def ppm(path):
    with open(path, "rb") as f:
        d = f.read()
    q = d.split(b"\n", 3)
    w, h = map(int, q[1].split())
    return w, h, q[3]


def diff_region(a, b, x0, y0, x1, y1, step=2):
    """Count differing pixels between two PPMs in a rectangle (sampled)."""
    w, _h, A = ppm(a)
    _, _h2, B = ppm(b)
    n = 0
    for y in range(y0, y1, step):
        row = y * w
        for x in range(x0, x1, step):
            j = (row + x) * 3
            if A[j:j + 3] != B[j:j + 3]:
                n += 1
    return n


# --- the tiny test framework ------------------------------------------

_TESTS = []


def test(fn):
    _TESTS.append(fn)
    return fn


def run_module(path):
    name = os.path.splitext(os.path.basename(path))[0]
    print("== group: %s ==" % name, flush=True)
    try:
        g = Guest(name)
    except TestFailure as e:
        print("  FAIL  boot: %s" % e, flush=True)
        print("GROUP %s: 0 passed, 1 failed" % name, flush=True)
        sys.exit(1)
    passed = failed = 0
    try:
        for fn in _TESTS:
            label = fn.__name__.replace("_", " ")
            t0 = time.time()
            try:
                fn(g)
                print("  PASS  %-26s (%4.1fs)" % (label, time.time() - t0), flush=True)
                passed += 1
            except TestFailure as e:
                print("  FAIL  %-26s %s" % (label, e), flush=True)
                g.dump("fail_" + fn.__name__)
                failed += 1
            except Exception as e:  # harness bug: report, keep going
                print("  ERROR %-26s %r" % (label, e), flush=True)
                g.dump("fail_" + fn.__name__)
                failed += 1
    finally:
        g.close()
    print("GROUP %s: %d passed, %d failed" % (name, passed, failed), flush=True)
    sys.exit(0 if failed == 0 else 1)

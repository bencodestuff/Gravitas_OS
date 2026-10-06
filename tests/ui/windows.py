#!/usr/bin/env python3
"""Window manager: discovery, focus, routing, close protocol.

The kernel starts the WM at boot, so this group uses it directly (no second WM
would be allowed to claim the display).
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from harness import test, run_module


@test
def stop_boot_terminal(g):
    # The boot terminal is a 646x400 window over everything; it would swallow
    # the title/close clicks these tests aim at their own windows.
    m = re.search(r"terminal as task (\d+)", g.log())
    if not m:
        g.fail("no boot terminal to stop")
    g.shell("kill " + m.group(1))
    g.wait_for("kill: terminated")


@test
def wm_lifecycle(g):
    g.wait_for("display authority")
    g.shell("bg win.elf")
    g.wait_for(r"window #[0-9]+ 'win")
    g.wait_for(r"wm: title #[0-9]+ 'Gravity demo'")   # client title over IPC
    w = g.await_window("win")

    n = g.count("wm: focus task")
    g.click_title(w)
    g.wait_more("wm: focus task", n + 1)
    g.key("a")
    g.wait_for(r"win: key 'a'")

    # Typematic repeat: a held key is re-delivered by the kernel.
    g.hold("a", 900)
    g.wait_count(r"win: key 'a'", 4)

    # A body click is forwarded to the client.
    g.click_body(w)
    g.wait_for(r"win: click [0-9]+,[0-9]+")

    # Alt+Tab from the desktop (focus on the console) cycles focus to a window.
    g.desktop()
    n = g.count("wm: focus task")
    g.alt("tab")
    g.wait_more("wm: focus task", n + 1)

    g.click_close(w)
    g.wait_window_gone("win")


@test
def cooperative_close(g):
    g.shell("bg closetest.elf")
    g.wait_for(r"window #[0-9]+ 'closetest")
    g.wait_for(r"wm: title #[0-9]+ 'Close test'")
    w = g.await_window("closetest")

    g.click_close(w)
    g.wait_for("closetest: vetoed")
    g.wait_for(r"wm: title #[0-9]+ 'Close test .vetoed.'")   # live retitle

    g.click_close(w)
    g.wait_for("closetest: closing")
    g.wait_window_gone("closetest")


@test
def force_close_backstop(g):
    # A client that ignores the close request (rawwin never reads its mailbox)
    # is force-killed once the grace period expires.
    g.shell("bg rawwin.elf")
    g.wait_for(r"window #[0-9]+ 'rawwin")
    g.wait_for("rawwin: up as")
    w = g.await_window("rawwin")

    g.click_close(w)
    g.wait_for(r"wm: close timeout #[0-9]+")
    g.wait_window_gone("rawwin")


if __name__ == "__main__":
    run_module(__file__)

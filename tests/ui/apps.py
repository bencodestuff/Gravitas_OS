#!/usr/bin/env python3
"""Desktop apps: the editor, the launcher menu, terminals, files, images, popups."""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from harness import diff_region, test, run_module


@test
def editor(g):
    g.shell("bg edit.elf /tmp/etest.txt")
    g.wait_for(r"window #[0-9]+ 'edit")
    g.wait_for(r"wm: title #[0-9]+ 'edit: etest\.txt'")
    w = g.await_window("edit")

    n = g.count("wm: focus task")
    g.click_title(w)
    g.wait_more("wm: focus task", n + 1)

    g.type("hello world"); g.key("ret"); g.type("second line")
    g.key("home"); g.type("x ")          # Home + prefix: exercises Home
    g.ctrl("s")
    g.wait_for(r"edit: saved /tmp/etest\.txt")
    # "hello world\nx second line" is exactly 25 bytes: a mismatch means a
    # dropped or misplaced keystroke (Home included).
    g.wait_for(r"edit: saved /tmp/etest\.txt \(25 bytes\)")
    g.ctrl("q")                           # saved: exits, WM refocuses the console
    g.wait_window_gone("edit")

    g.shell("cat /tmp/etest.txt")
    g.wait_for("x second line")           # the shell reads back what was typed


@test
def launcher_menu(g):
    before = os.path.join(g.art, "menu_before.ppm")
    after = os.path.join(g.art, "menu_after.ppm")
    g.screendump(before)
    g.click(*g.apps_button())
    g.screendump(after)

    # The menu must actually be PAINTED, not merely opened in WM state: a
    # commit that targets the wrong surface id is silently dropped, leaving the
    # menu in memory but never composited.
    x0, y0, x1, y1 = g.menu_rect()
    if diff_region(before, after, x0 + 2, y0 + 2, x1 - 2, y1 - 2) <= 100:
        g.fail("the Apps menu was opened but not painted (overlay commit dropped?)")

    g.desktop()   # dismiss the menu so the next test's launch() starts closed


@test
def terminal(g):
    n = g.count(r"wm: window #[0-9]+ 'term")
    g.launch("Terminal")
    g.wait_for("wm: launch 'term.elf'")
    g.wait_count(r"wm: window #[0-9]+ 'term", n + 1)

    # Alt+Tab cycles focus to a window.
    f = g.count("wm: focus task")
    g.alt("tab")
    g.wait_more("wm: focus task", f + 1)

    term = g.await_window("term")          # the launched terminal (created last)
    g.click_title(term)

    # Keying 'ls' reaches only the focused task, so a change inside the window
    # (and not the console behind it) proves the terminal input path.
    before = os.path.join(g.art, "term_before.ppm")
    after = os.path.join(g.art, "term_after.ppm")
    g.screendump(before)
    g.shell("ls")
    g.wait_for(r"User task 'ls\.elf' created")
    time.sleep(1.0)
    g.screendump(after)
    changed = diff_region(before, after,
                          term["x"] + 2, term["y"] + 2,
                          term["x"] + term["w"] - 2, term["y"] + term["h"] - 2)
    if changed <= 200:
        g.fail("terminal did not render the hosted shell's output (keys missed it?)")


@test
def terminal_interrupt(g):
    # The previous test focused the launched terminal, so typing goes to its
    # hosted shell. Ctrl+C must reach that shell's foreground job through the
    # terminal's CONSOLE_WRITE — the kernel applies the interrupt rule there.
    n = g.count(r"User task 'hang\.elf' created")
    g.shell("run hang.elf")
    g.wait_count(r"User task 'hang\.elf' created", n + 1)
    g.ctrl("c")
    g.wait_for(r"Task 'hang\.elf' terminated \(killed\)")


@test
def file_browser(g):
    g.launch("Files")
    g.wait_for("wm: launch 'files.elf'")
    g.wait_count(r"wm: window #[0-9]+ 'files", 1)
    g.wait_for(r"wm: title #[0-9]+ 'Files'")


@test
def image_viewer(g):
    g.launch("Image viewer")
    g.wait_for("wm: launch 'imageview.elf'")
    g.wait_count(r"wm: window #[0-9]+ 'imageview", 1)
    g.wait_for(r"wm: title #[0-9]+ 'Images'")
    g.wait_for(r"imageview: /Pictures/")


@test
def popup(g):
    g.desktop()                            # focus the console (dismisses nothing)
    g.shell("bg popuptest.elf")
    g.wait_for("popuptest: popup open")
    g.wait_for(r"wm: popup #[0-9]+")
    g.desktop()                            # click outside -> dismiss
    g.wait_for("popuptest: popup closed")


if __name__ == "__main__":
    run_module(__file__)

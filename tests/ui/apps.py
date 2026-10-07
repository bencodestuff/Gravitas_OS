#!/usr/bin/env python3
"""Desktop apps: the editor, the launcher menu, terminals, files, images, popups."""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from harness import diff_region, ppm, test, run_module


def thumb_top(shot, win):
    """The y of the topmost scrollbar-thumb pixel in a terminal's strip."""
    w, h, data = ppm(shot)
    x0 = win["x"] + 80 * 8          # COLS * CELL_W: the strip starts here
    best = None
    for y in range(win["y"], win["y"] + 25 * 16):   # ROWS * CELL_H
        for x in range(x0, x0 + 6):                 # SB_W
            j = (y * w + x) * 3
            if (data[j], data[j + 1], data[j + 2]) == (0x6A, 0x7A, 0x8A):
                if best is None or y < best:
                    best = y
    return best


@test
def stop_boot_terminal(g):
    # The boot terminal and a launched one overlap, which makes "the terminal
    # under the pointer" ambiguous. The tests below launch their own.
    m = re.search(r"terminal as task (\d+)", g.log())
    if m:
        g.shell("kill " + m.group(1))
        g.wait_for("kill: terminated")


@test
def editor(g):
    g.shell("bg edit.elf /etest.txt")
    g.wait_for(r"window #[0-9]+ 'edit")
    g.wait_for(r"wm: title #[0-9]+ 'edit: etest\.txt'")
    w = g.await_window("edit")

    n = g.count("wm: focus task")
    g.click_title(w)
    g.wait_more("wm: focus task", n + 1)

    g.type("hello world"); g.key("ret"); g.type("second line")
    g.key("home"); g.type("x ")          # Home + prefix: exercises Home
    g.ctrl("s")
    g.wait_for(r"edit: saved /etest\.txt")
    # "hello world\nx second line" is exactly 25 bytes: a mismatch means a
    # dropped or misplaced keystroke (Home included).
    g.wait_for(r"edit: saved /etest\.txt \(25 bytes\)")
    g.ctrl("q")                           # saved: exits, WM refocuses the console
    g.wait_window_gone("edit")

    g.shell("cat /etest.txt")
    g.wait_for("x second line")           # the shell reads back what was typed


@test
def editor_selection(g):
    # The selection + the file clipboard: Shift+Home selects the line,
    # Ctrl+Shift+C copies it, Ctrl+Shift+V pastes it at the caret. The Shift is
    # what keeps Ctrl+Shift+C apart from Ctrl+C (the interrupt key).
    g.shell("bg edit.elf /seltest.txt")
    g.wait_for(r"window #[0-9]+ 'edit")
    g.wait_for(r"wm: title #[0-9]+ 'edit: seltest\.txt'")
    w = g.await_window("edit")

    n = g.count("wm: focus task")
    g.click_title(w)
    g.wait_more("wm: focus task", n + 1)

    g.type("hello")
    g.chord("shift", "home")         # anchor at the caret, move to line start
    g.chord("ctrl", "shift", "c")    # copy the selection
    g.key("end")                     # caret to the end (drops the selection)
    g.chord("ctrl", "shift", "v")    # paste it
    g.key("ret")                     # a newline, so cat's output is its own line
    g.ctrl("s")
    g.wait_for(r"edit: saved /seltest\.txt \(11 bytes\)")
    g.ctrl("q")
    g.wait_window_gone("edit")

    g.shell("cat /seltest.txt")
    g.wait_for(r"^hellohello$")      # copied once, pasted once


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
    g.wait_for(r"Task 'hang\.elf' (terminated \(killed\)|exited)")


@test
def terminal_scroll(g):
    # The scrollbar thumb tracks the scrollback: wheel up moves it up, wheel
    # down brings it back, and clicking the strip jumps. This is also the
    # regression test for the wheel's sign (the PS/2 wheel byte is a 4-bit
    # two's-complement delta that must be sign-extended).
    term = g.await_window("term")

    # Give the hosted shell some scrollback.
    n = g.count(r"User task 'flood\.elf' created")
    g.shell("run flood.elf")
    g.wait_count(r"User task 'flood\.elf' created", n + 1)
    time.sleep(1.5)                       # let the output arrive and render

    # The wheel goes to the window under the pointer, so point at the body.
    g.move_to(term["x"] + 100, term["y"] + 100)

    def shot(name):
        p = os.path.join(g.art, name)
        g.screendump(p)
        return thumb_top(p, term)

    y0 = shot("scroll_bottom.ppm")
    if y0 is None:
        g.fail("no scrollbar thumb after flood (no scrollback?)")

    y1 = None
    g.wheel(3)                            # up
    y1 = shot("scroll_up.ppm")
    if y1 is None or y1 >= y0:
        g.fail("wheel up did not scroll back (thumb %s -> %s)" % (y0, y1))

    g.wheel(-3)                           # down
    y2 = shot("scroll_down.ppm")
    if y2 is None or y2 <= y1:
        g.fail("wheel down did not scroll forward (thumb %s -> %s)" % (y1, y2))

    # Clicking near the top of the strip jumps to the top of the history.
    g.click(term["x"] + 80 * 8 + 3, term["y"] + 20)
    y3 = shot("scroll_click.ppm")
    if y3 is None or y3 - term["y"] > 8:
        g.fail("scrollbar click did not jump to the top (thumb at %s, window %s)"
               % (y3, term["y"]))


@test
def terminal_lua(g):
    # The interpreter, hosted by the terminal: typing must reach the hosted
    # shell's foreground job (the console binding is inherited, and
    # CONSOLE_WRITE routes to the foreground reader). Lua's own output goes to
    # the window, not the serial log, so the check is a window repaint.
    term = g.await_window("term")
    g.click_title(term)

    before = os.path.join(g.art, "lua_term_before.ppm")
    g.screendump(before)

    n = g.count(r"User task 'lua\.elf' created")
    g.shell("run lua.elf")
    g.wait_count(r"User task 'lua\.elf' created", n + 1)
    time.sleep(1.0)                        # the banner renders in the window

    g.shell("print(6*7)")                  # into the terminal -> the hosted lua
    time.sleep(1.0)

    after = os.path.join(g.art, "lua_term_after.ppm")
    g.screendump(after)
    changed = diff_region(before, after,
                          term["x"] + 2, term["y"] + 2,
                          term["x"] + term["w"] - 2, term["y"] + term["h"] - 2)
    if changed <= 100:
        g.fail("typing did not reach the hosted lua (window unchanged)")

    g.ctrl("c")
    g.wait_for(r"Task 'lua\.elf' (terminated \(killed\)|exited)")


@test
def terminal_paste(g):
    # The terminal pastes the file clipboard into its hosted shell's stdin, the
    # same path as typing — so a pasted command runs. Focus is the launched
    # terminal from the tests above.
    term = g.await_window("term")
    g.click_title(term)

    # Put a command in the clipboard (Lua writes the file for us).
    g.shell("run lua.elf -e io.open('/tmp/.clipboard','w'):write('ls\\n')")
    g.wait_for(r"\[shell\] program finished")

    n = g.count(r"User task 'ls\.elf' created")
    g.chord("ctrl", "shift", "v")   # paste "ls\n" -> the shell runs it
    g.wait_count(r"User task 'ls\.elf' created", n + 1)


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


@test
def window_geometry(g):
    # --geometry sizes the window; the WM still owns where it goes.
    g.desktop()
    g.shell("bg files.elf --geometry 500x400")
    g.wait_for(r"bg: started 'files\.elf' as task [0-9]+")
    g.wait_for(r"window #[0-9]+ 'files\.elf' 500x400")


@test
def ide_window(g):
    # The IDE is ONE window: the explorer, the editor and the terminal are panes
    # inside it, not three top-level windows. It opens with a file already in a
    # tab (the tests below keep using this window).
    g.desktop()
    n = g.count(r"wm: window #")
    g.shell("bg ide.elf /tmp/ide.lua")
    g.wait_for(r"ide: open /tmp/ide\.lua")
    g.wait_for(r"wm: title #[0-9]+ 'Gravitas IDE'")
    if g.count(r"wm: window #") != n + 1:
        g.fail("the IDE opened more than one window")


@test
def ide_run(g):
    # A file opened in the editor runs in the terminal pane on F5: the IDE saves
    # it first, then types the command into the shell it hosts below.
    w = g.await_window("Gravitas IDE")
    g.click_title(w)                      # the IDE takes the keys
    g.ctrl("2")                           # focus the editor pane
    g.type("print('ide-run')")
    time.sleep(0.4)

    before = os.path.join(g.art, "ide_run_before.ppm")
    g.screendump(before)

    n = g.count(r"User task 'lua\.elf' created")
    g.key("f5")
    g.wait_for(r"ide: run run lua\.elf /tmp/ide\.lua")
    g.wait_count(r"User task 'lua\.elf' created", n + 1)
    time.sleep(1.2)

    after = os.path.join(g.art, "ide_run_after.ppm")
    g.screendump(after)
    # The terminal is the pane at the bottom of the window: the program's output
    # has to be painted there, not merely run.
    x0 = w["x"] + 4
    y0 = w["y"] + w["h"] - 218
    x1 = w["x"] + w["w"] - 4
    y1 = w["y"] + w["h"] - 26
    if diff_region(before, after, x0, y0, x1, y1) <= 100:
        g.fail("F5 ran the file but the terminal pane did not paint the output")


@test
def ide_explorer(g):
    # The tree feeds the centre pane: opening a file makes a tab in the editor,
    # not another window.
    w = g.await_window("Gravitas IDE")
    g.click_title(w)

    n = g.count(r"wm: window #")
    # The tree starts at "/": bin/ fonts/ lua/ Pictures/ tmp/ apps hello.txt.
    row = lambda i: w["y"] + 2 + i * 20 + 10
    g.click(w["x"] + 100, row(2))         # lua/ — a directory opens on one click
    time.sleep(0.5)
    g.click(w["x"] + 100, row(1))         # hello.lua — select
    time.sleep(0.3)
    g.click(w["x"] + 100, row(1))         # ... and open

    g.wait_for(r"ide: open /lua/hello\.lua")
    if g.count(r"wm: window #") != n:
        g.fail("opening a file from the tree made a new window")
    g.wait_for(r"wm: title #[0-9]+ 'Gravitas IDE — hello\.lua'")


@test
def ide_close_tab(g):
    # Each tab has a close box at its right edge. The tabs are "ide.lua" then
    # "hello.lua", so the second tab's box is what this clicks.
    w = g.await_window("Gravitas IDE")
    g.click_title(w)

    n = g.count(r"ide: close ")
    g.click(w["x"] + 262 + 2 + 80 + 2 + 96 - 6, w["y"] + 10)  # tab 1's close box
    g.wait_count(r"ide: close ", n + 1)
    g.wait_for(r"ide: close /lua/hello\.lua")
    g.wait_for(r"wm: title #[0-9]+ 'Gravitas IDE — ide\.lua'")


if __name__ == "__main__":
    run_module(__file__)

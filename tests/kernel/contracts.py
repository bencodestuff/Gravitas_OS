#!/usr/bin/env python3
"""Surface + damage contracts, on unmanaged surfaces.

The boot window manager is stopped first: it would decorate and reposition
anything these tests create.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from harness import test, run_module


@test
def unmanaged_surfaces(g):
    want = g.count("kill: terminated")
    found = 0
    for pat in (r"window manager as task (\d+)", r"terminal as task (\d+)"):
        m = re.search(pat, g.log())
        if not m:
            continue
        g.shell("kill " + m.group(1))
        want += 1
        g.wait_count("kill: terminated", want)
        found += 1
    if found < 2:
        g.fail("could not find the boot WM/terminal tasks to stop")


@test
def damage_compositor(g):
    # A small window's lifecycle must repaint only its rectangles, not the
    # screen per operation. damagetest samples the compositor's pixel counter
    # around just its surface ops, so console scrolling cannot pollute it.
    g.shell("run damagetest.elf")
    g.wait_for("damagetest done")
    dam = g.last_int(r"damagetest: window damage (\d+) px")
    if dam is None:
        g.fail("damagetest reported no damage figure")
    # The window is 240x160 (38,400 px). Create + moves + commits + destroy
    # stays well under 2M with damage tracking; a full-screen repaint per op
    # would be several million (1,024,000 px each).
    if dam >= 2000000:
        g.fail("compositor painted too much for a small window (%d px)" % dam)


@test
def surface_contract(g):
    g.shell("run surfacetest.elf")
    g.wait_for("surface contract ok")


if __name__ == "__main__":
    run_module(__file__)

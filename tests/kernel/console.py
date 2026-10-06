#!/usr/bin/env python3
"""Console: boot identity, the shipped font, the /bin layout, the shell."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from harness import test, run_module


@test
def boot_identity(g):
    # _boot waited for the banner, the VFS self-test, the shell prompt and the
    # terminal. The shipped console font must be the PSF2, not the fallback.
    if g.count("keeping 8x8"):
        g.fail("font fallback triggered (expected PSF2 8x16)")
    g.wait_for(r"\[font\] loaded .*8x16")


@test
def bin_layout(g):
    # Test programs live in /bin/tests (and are still found by a bare name), so
    # /bin reads like a real system.
    mark = g.mark()
    g.shell("ls /bin")
    g.wait_for(r"\[shell\] program finished")
    listing = g.since(mark)
    for stray in ("crash.elf", "damagetest.elf", "surfhelper.elf"):
        if stray in listing:
            g.fail("/bin still lists the test program %s:\n%s" % (stray, listing))
    if "tests" not in listing:
        g.fail("/bin/tests is not listed:\n%s" % listing)


@test
def backspace_erase(g):
    # 'helpq' + BS -> 'help': the erase path, proven by the help text.
    for ch in "helpq":
        g.key(ch)
    g.raw("0e", "8e")     # backspace (sendkey bspc is dropped by QEMU)
    g.key("ret")
    g.wait_for("commands are programs")


@test
def cat_file(g):
    # Bare-name spawn + VFS path fallback + a file read.
    g.shell("cat hello.txt")
    g.wait_for("Hello from the Gravitas tar")


@test
def console_scroll(g):
    # flood prints 150 numbered lines, forcing the cell layer to scroll far
    # past the screen.
    g.shell("run flood.elf")
    g.wait_for("flood line 150")
    g.wait_for("flood done")


if __name__ == "__main__":
    run_module(__file__)

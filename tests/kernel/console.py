#!/usr/bin/env python3
"""Console: boot identity, the shipped font, the /bin layout, the shell."""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from harness import ppm, test, run_module


@test
def boot_identity(g):
    # _boot waited for the banner, the VFS self-test, the shell prompt and the
    # terminal. The shipped console font must be the PSF2, not the fallback.
    if g.count("keeping 8x8"):
        g.fail("font fallback triggered (expected PSF2 8x16)")
    g.wait_for(r"\[font\] loaded .*8x16")

    # The Block layer's self-test runs on a scratch RAM disk of its own; the
    # data disk below is a real ATA drive the harness attaches.
    g.wait_for(r"\[Block\] self-test ok")
    g.wait_for(r"\[ATA\] ata0: [0-9]+ sectors, LBA(28|48)")
    # A second drive on the controller is scratch, and its self-test is the only
    # place the ATA write path runs.
    g.wait_for(r"\[Block\] scratch drive self-test ok")

    # Interrupt routing is taken over before interrupts are enabled: the PIC
    # remapped (so IRQ0-7 are not vectors 8-15), and the IOAPIC silenced.
    g.wait_for(r"\[PIC\] Remapped; interrupts on\.")
    g.wait_for(r"\[IOAPIC\] [0-9]+ entries, [0-9]+ unmasked by the firmware, "
               r"now all masked")

    # The read-only ext2 image: a real on-disk filesystem, mounted from a RAM
    # disk seeded with the Limine module.
    g.wait_for(r"\[Ext2\] [0-9]+-byte blocks, [0-9]+ blocks, [0-9]+ inodes, "
               r"[0-9]+ group \(read-only\)")


@test
def wall_clock(g):
    # The RTC is read once at boot; the epoch must be plausible (after 2001).
    m = re.search(r"\[RTC\] boot epoch: (\d+)", g.log())
    if m is None:
        g.fail("the RTC was not read at boot (no boot epoch)")
    if int(m.group(1)) < 1000000000:
        g.fail("RTC boot epoch looks wrong: " + m.group(1))


@test
def taskbar_clock(g):
    # The WM shows the RTC-backed time at the right end of the taskbar.
    g.wait_for(r"wm: clock \d\d:\d\d")

    # Give the compositor a moment to present the first paint.
    time.sleep(1.0)
    shot = os.path.join(g.art, "taskbar.ppm")
    g.screendump(shot)

    w, h, data = ppm(shot)
    bar_bg = (0x17, 0x19, 0x1C)          # the WM's BAR_BG
    hits = 0
    for y in range(h - 24 + 2, h - 2):   # inside the 24 px bar, every row
        row = y * w
        for x in range(w - 52, w - 6):   # the clock's slot at the right end
            j = (row + x) * 3
            if (data[j], data[j + 1], data[j + 2]) != bar_bg:
                hits += 1
    if hits < 20:
        g.fail("the taskbar clock is not painted (%d non-background pixels)" % hits)


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


@test
def shell_history(g):
    # The console hands the shell a byte stream, so the arrows arrive as the
    # readline codes: Up recalls the previous command, ^U kills the line.
    n = g.count(r"User task 'ls\.elf' created")
    g.shell("ls")
    g.wait_count(r"User task 'ls\.elf' created", n + 1)

    g.key("up")           # recall it
    g.key("ret")          # and run it again
    g.wait_count(r"User task 'ls\.elf' created", n + 2)

    # ^U empties the line, so only what is typed afterwards runs.
    g.type("this is not a command")
    g.ctrl("u")
    g.type("ls")
    g.key("ret")
    g.wait_count(r"User task 'ls\.elf' created", n + 3)


@test
def shell_line_edit(g):
    # The console's '\b' moves without erasing (as a terminal's does), so the
    # shell's reader can keep a cursor anywhere in the line: Home puts it at the
    # start, and what is typed next goes in *there*.
    g.type("t hello.txt")
    g.key("home")
    g.type("ca")                      # -> "cat hello.txt"

    # A redraw returns to the start of the line — it must not start a new one.
    # (The prompt is printed once with its newline; the text a redraw reprints
    # must not carry it, or every keystroke would scroll. Read raw: a translated
    # '\r' would look like a newline, which is the whole difference.)
    end = time.time() + 10
    while time.time() < end:
        if re.search(r"\r/ root@gravitas# cat hello\.txt", g.raw_log()):
            break
        time.sleep(0.05)
    else:
        g.fail("redrawing the line started a new one (\\r was followed by \\n)")

    g.key("ret")
    g.wait_for("Hello from the Gravitas tar")


@test
def ls_long_metadata(g):
    # ls -l is only possible because a node carries an identity and metadata: a
    # type and mode, an inode and a size. The console shell prints into the
    # serial log, so this reads the listing the way a user would.
    g.shell("ls -l /bin")
    g.wait_for(r"[d-][rwx-]{9}\s+[0-9]+\s+[0-9]+\s+tests/")


if __name__ == "__main__":
    run_module(__file__)

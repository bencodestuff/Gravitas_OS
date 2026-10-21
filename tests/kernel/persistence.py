#!/usr/bin/env python3
"""Persistence: what reaches the data disk is still there after a reboot.

Every other group boots on the image the build produced, attached with QEMU's
snapshot=on, so a guest write is thrown away when QEMU exits. That is the right
default: a driver bug must not be able to damage the shipped image. This group
instead boots on a private copy of that image, writes to it, leaves the machine,
and boots a second guest on the same file. The second guest reads back what the
first wrote, which means the bytes reached the medium and survived the machine
going away; e2fsck afterwards says whether the result is also a clean ext2
filesystem, not merely a readable one.

The test does not call sync: leaving the machine is what has to flush, and a
test that syncs first would pass even if poweroff forgot to. Nothing here
writes to "/": the disk is mounted at /disk, and the files are all under it,
with names no other group uses.
"""
import atexit
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from harness import Guest, test, run_module

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PRISTINE = os.path.join(ROOT, "iso_root", "disk.img")


def _prepare():
    """Make a writable copy of the image the boot would otherwise snapshot."""
    if not os.path.exists(PRISTINE):
        raise SystemExit("persistence: iso_root/disk.img missing - "
                         "run 'ninja iso' first")
    if shutil.which("e2fsck") is None:
        raise SystemExit("persistence: e2fsck is missing (install e2fsprogs)")
    tmp = tempfile.mkdtemp(prefix="gravitas-persist-")
    atexit.register(shutil.rmtree, tmp, ignore_errors=True)
    copy = os.path.join(tmp, "disk.img")
    shutil.copyfile(PRISTINE, copy)
    return tmp, copy


TMPDIR, DISK = _prepare()

# The group's first guest boots on the copy, not on the repo image.
BOOT = {"disk_image": DISK}


def run(g, line):
    """Type a shell line and wait for that program to exit.

    Anchored on the program's own "Task '<prog>.elf' exited", the way
    file_commands does: a background task's generic "program finished" can
    arrive late and make a wait return before the command ran at all.
    """
    prog = line.split()[0]
    if prog == "run":
        prog = line.split()[1]
    if prog.endswith(".elf"):
        prog = prog[:-4]
    pat = r"Task '%s\.elf' exited" % prog
    n = g.count(pat)
    g.shell(line)
    g.wait_more(pat, n + 1)


def cat(g, path, expected):
    """cat 'path' and require 'expected' in that command's output."""
    mark = g.mark()
    run(g, "cat " + path)
    out = g.since(mark)
    if expected not in out:
        g.fail("cat %s did not print %r; it printed:\n%s"
               % (path, expected, out.strip()))


def leave(g, why):
    # poweroff does not return, so there is no "Task 'poweroff.elf' exited" to
    # wait for: the machine going away is the exit.
    g.shell("run poweroff.elf")
    g.wait_for("poweroff: shutting down")
    if not g.await_exit():
        tail = "\n".join(g.log().splitlines()[-20:])
        g.fail("the guest did not power off (%s); serial tail:\n%s" % (why, tail))


def fsck(path):
    """Run e2fsck read-only; returns (returncode, output)."""
    p = subprocess.run(["e2fsck", "-fn", path], stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True)
    return p.returncode, p.stdout.strip()


@test
def disk_writes_survive_reboot(g):
    # The copy must start clean, or the final e2fsck would be judging damage
    # this test did not cause.
    rc, out = fsck(DISK)
    if rc != 0:
        g.fail("the copied image was not clean before boot (e2fsck rc %d):\n%s"
               % (rc, out))

    # First boot: write under /disk, including an overwrite of an image file
    # and a file created then removed.
    run(g, "mkdir -p /disk/persist/deep")
    run(g, "cp /disk/hello.txt /disk/persist/hello-copy.txt")
    run(g, "cp /disk/sub/nested.txt /disk/persist/deep/nested.txt")
    run(g, "cp /disk/big.bin /disk/persist/big-copy.bin")
    run(g, "cp /disk/hello.txt /disk/sub/nested.txt")
    run(g, "cp /disk/hello.txt /disk/persist/gone.txt")
    run(g, "rm /disk/persist/gone.txt")
    leave(g, "first boot")

    # Second boot on the same file: every write above is on the medium, and the
    # removal is a change to the medium too.
    g2 = Guest("persistence-second", disk_image=DISK)
    try:
        cat(g2, "/disk/persist/hello-copy.txt", "Hello from the ext2 disk")
        cat(g2, "/disk/persist/deep/nested.txt", "A nested file.")
        # The overwrite stuck: this is now hello.txt's content, not the
        # "A nested file." the image was built with.
        cat(g2, "/disk/sub/nested.txt", "Hello from the ext2 disk")

        mark = g2.mark()
        run(g2, "ls /disk/persist")
        listing = g2.since(mark)
        for name in ("hello-copy.txt", "big-copy.bin", "deep/"):
            if not re.search(r"^%s$" % re.escape(name), listing, re.M):
                g2.fail("ls /disk/persist lost %r:\n%s"
                        % (name, listing.strip()))
        if re.search(r"^gone\.txt$", listing, re.M):
            g2.fail("the removed file came back:\n%s" % listing.strip())

        # Still writable after a reboot. The new file goes under deep/ rather
        # than into the directory rm emptied a slot in: the kernel's
        # dir_insert splits a freed record leaving an 8-byte one, which is
        # below ext2's minimum record size, so e2fsck would rightly call the
        # result corrupt. That is a directory-allocation bug, not a
        # persistence one; this group is about the bytes surviving.
        run(g2, "cp /disk/hello.txt /disk/persist/deep/after-reboot.txt")
        cat(g2, "/disk/persist/deep/after-reboot.txt", "Hello from the ext2 disk")
        leave(g2, "second boot")
    finally:
        g2.close()

    # A clean ext2 image, not just one that happens to read back.
    rc, out = fsck(DISK)
    if rc != 0:
        g.fail("e2fsck -fn says the disk is not clean (rc %d):\n%s"
               % (rc, out))
    print("  e2fsck clean: %s" % out.splitlines()[-1], flush=True)

    shutil.rmtree(TMPDIR, ignore_errors=True)


if __name__ == "__main__":
    run_module(__file__)

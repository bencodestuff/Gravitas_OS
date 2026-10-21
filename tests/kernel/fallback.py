#!/usr/bin/env python3
"""The no-drive path: the disk is only the Limine module.

Every other group runs with a real ATA drive attached, so the ATA driver is
covered everywhere. This one boots the same ISO with no drive, which is what a
machine without an ATA disk looks like, and checks the guest still comes up and
still mounts ext2 from the module copied into RAM. Without this the fallback is
only ever exercised by hand.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from harness import test, run_module

BOOT = {"disk": False}


@test
def disk_fallback(g):
    g.wait_for(r"\[Disk\] no virtio or ATA drive; disk\.img copied into RAM")
    g.wait_for(r"\[Ext2\] [0-9]+-byte blocks, [0-9]+ blocks, [0-9]+ inodes, "
               r"[0-9]+ groups? \(read-write\)")
    if g.count(r"\[ATA\] "):
        g.fail("an ATA device appeared with no drive attached")
    if g.count(r"\[Virtio\] "):
        g.fail("a virtio device appeared with no drive attached")


@test
def disk_content(g):
    # The module copy is the same image, so the same files read back.
    g.shell("cat /disk/hello.txt")
    g.wait_for("Hello from the ext2 disk")


if __name__ == "__main__":
    run_module(__file__)

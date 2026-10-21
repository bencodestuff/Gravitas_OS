#!/usr/bin/env python3
"""virtio-blk: the DMA driver behind the same Block interface.

This group boots with virtio-blk devices instead of the classic IDE controller,
so the data disk and the scratch drive both go through src/kernel/dev/virtio.cpp
over DMA. The same Block self-test and the same ext2 image must work exactly as
they do over ATA PIO — the point of the step is that nothing above Block knows
which controller it is talking to.

The harness attaches no IDE controller here, so if an [ATA] line appeared the
guest would be reaching a disk this group did not attach.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from harness import test, run_module

# A virtio data disk, plus a second virtio drive the kernel treats as scratch:
# its self-test is the only place the DMA write path runs.
BOOT = {"virtio": True, "scratch": True}


@test
def virtio_boot(g):
    # The Block layer's self-test runs on its own RAM disk.
    g.wait_for(r"\[Block\] self-test ok")
    # The data disk is a virtio-blk device, found on the PCI bus.
    g.wait_for(r"\[Virtio\] virtio0: [0-9]+ sectors, queue [0-9]+")
    # The second drive is scratch, and its self-test is the only place the
    # virtio write path (and so a DMA write) runs.
    g.wait_for(r"\[Block\] scratch drive self-test ok")

    if g.count(r"\[ATA\] "):
        g.fail("an ATA device appeared on a virtio-only machine")
    if g.count(r"\[Disk\] no virtio or ATA drive"):
        g.fail("the data disk fell back to the RAM disk")

    # The same ext2 image, now mounted read-write over virtio.
    g.wait_for(r"\[Ext2\] [0-9]+-byte blocks, [0-9]+ blocks, [0-9]+ inodes, "
               r"[0-9]+ groups? \(read-write\)")


@test
def virtio_disk_content(g):
    g.shell("cat /disk/hello.txt")
    g.wait_for("Hello from the ext2 disk")


if __name__ == "__main__":
    run_module(__file__)

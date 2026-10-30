#!/usr/bin/env python3
"""Hostile ext2 images: geometry and block numbers must not be trusted.

A mounted filesystem is attacker-controlled data. The kernel's ext2 driver
reads geometry and block addresses straight out of the image, so every one of
them has to be checked before it is used: a bad block number must never become
a read or a write of a sector that belongs to something else, and a bad bitmap
index must never leave a 4 KB stack buffer.

Each test boots a private copy of the shipped image with one field patched on
the host. The assertions are deliberately about the machine staying up and the
failure being clean: a refused mount still boots, a corrupt block pointer still
leaves a usable shell, and neither ends in a kernel fault. The bitmap-index
clamp in free_block/free_inode is not reachable through a mounted image any
more (mount refuses a group larger than its bitmap), so it has no guest test;
the mount refusal below is what covers that class of image.
"""
import atexit
import os
import shutil
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from harness import Guest, test, run_module

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PRISTINE = os.path.join(ROOT, "iso_root", "disk.img")

SUPERBLOCK = 1024
S_INODES_PER_GROUP = SUPERBLOCK + 40
S_BLOCKS_COUNT = SUPERBLOCK + 4
S_FIRST_DATA_BLOCK = SUPERBLOCK + 20
S_LOG_BLOCK_SIZE = SUPERBLOCK + 24
S_INODE_SIZE = SUPERBLOCK + 88


def _prepare():
    """A writable private copy of the image the boot would otherwise snapshot."""
    if not os.path.exists(PRISTINE):
        raise SystemExit("hostile: iso_root/disk.img missing - "
                         "run 'ninja iso' first")
    tmp = tempfile.mkdtemp(prefix="gravitas-hostile-")
    atexit.register(shutil.rmtree, tmp, ignore_errors=True)
    copy = os.path.join(tmp, "disk.img")
    shutil.copyfile(PRISTINE, copy)
    return tmp, copy


TMPDIR, BAD_GEOMETRY = _prepare()

# Patch s_inodes_per_group far past what a 1 KB block's bitmap can describe.
with open(BAD_GEOMETRY, "r+b") as f:
    f.seek(S_INODES_PER_GROUP)
    f.write(struct.pack("<I", 65536))

# The group's first guest boots on the corrupt image, not on the repo copy.
BOOT = {"disk_image": BAD_GEOMETRY}


def _read_u32(path, off):
    with open(path, "rb") as f:
        f.seek(off)
        return struct.unpack("<I", f.read(4))[0]


def _read_u16(path, off):
    with open(path, "rb") as f:
        f.seek(off)
        return struct.unpack("<H", f.read(2))[0]


def _patch_u32(path, off, value):
    with open(path, "r+b") as f:
        f.seek(off)
        f.write(struct.pack("<I", value))


def _geometry(path):
    """The fields the mount validation itself reads (a clean image)."""
    bs = 1024 << _read_u32(path, S_LOG_BLOCK_SIZE)
    first_data = _read_u32(path, S_FIRST_DATA_BLOCK)
    return {
        "block_size": bs,
        "blocks_count": _read_u32(path, S_BLOCKS_COUNT),
        "first_data_block": first_data,
        "inode_size": _read_u16(path, S_INODE_SIZE),
        # Group descriptors sit in the block after the superblock's block.
        "gd_offset": (first_data + 1) * bs,
    }


def _inode_offset(path, geo, ino):
    """Byte offset of an inode, from the first group's inode table."""
    table = _read_u32(path, geo["gd_offset"] + 8)
    return table * geo["block_size"] + (ino - 1) * geo["inode_size"]


def _read_inode(path, geo, ino):
    with open(path, "rb") as f:
        f.seek(_inode_offset(path, geo, ino))
        return f.read(128)


def _find_inode(path, geo, dir_ino, name):
    """Inode number of a name in a directory, reading direct blocks only."""
    bs = geo["block_size"]
    inode = _read_inode(path, geo, dir_ino)
    blocks = struct.unpack_from("<15I", inode, 40)
    with open(path, "rb") as f:
        for blk in blocks[:12]:
            if blk == 0:
                continue
            f.seek(blk * bs)
            data = f.read(bs)
            p = 0
            while p + 8 <= bs:
                ino, rec_len, name_len, _ = struct.unpack_from("<IHBB", data, p)
                if rec_len < 8 or p + rec_len > bs:
                    break
                if (ino != 0 and name_len == len(name) and
                        data[p + 8:p + 8 + name_len] == name):
                    return ino
                p += rec_len
    return None


def run(g, line):
    """Type a shell line and wait for that program to exit."""
    prog = line.split()[0]
    pat = r"Task '%s\.elf' exited" % prog
    n = g.count(pat)
    g.shell(line)
    g.wait_more(pat, n + 1)


def clean(g):
    """A refused image or a contained error must not fault the kernel."""
    for marker in ("=== FAULT:", "[fault] kernel fault: halting",
                   "[fault] halting CPU"):
        if marker in g.log():
            g.fail("the kernel faulted on a corrupt image (%r)" % marker)


@test
def bad_geometry_refused(g):
    # The guest is already up, which is itself the no-hang assertion. The
    # superblock's inodes_per_group describes a bitmap larger than a block, so
    # mount must refuse the image rather than clamp a later index.
    g.wait_for(r"\[Ext2\] the disk is not ext2")
    g.wait_for(r"\[VFS\] Mounted / \(overlay: ramfs over tarfs\)\.")
    if g.count(r"\[Ext2\] [0-9]+-byte blocks"):
        g.fail("an image with an impossible inodes_per_group was mounted")
    clean(g)

    # The shell is still alive, and the refused disk is simply not there.
    n = g.count("commands are programs")
    g.shell("help")
    g.wait_count("commands are programs", n + 1)

    mark = g.mark()
    run(g, "ls /disk")
    out = g.since(mark)
    if "not a directory" not in out and "No such file" not in out:
        g.fail("a refused disk was still reachable; ls printed:\n%s"
               % out.strip())
    clean(g)


@test
def bad_group_descriptor_refused(g):
    # A group descriptor whose block_bitmap names a block past blocks_count is
    # not metadata this filesystem can use. The descriptor is read while the
    # root inode is loaded, so the mount has to refuse the image here.
    tmp, copy = _prepare()
    try:
        geo = _geometry(copy)
        _patch_u32(copy, geo["gd_offset"], geo["blocks_count"] + 100)

        g2 = Guest("hostile-gd", disk_image=copy)
        try:
            g2.wait_for(r"\[Ext2\] the disk is not ext2")
            if g2.count(r"\[Ext2\] [0-9]+-byte blocks"):
                g2.fail("an image with an out-of-range block_bitmap was mounted")
            clean(g2)
            n = g2.count("commands are programs")
            g2.shell("help")
            g2.wait_count("commands are programs", n + 1)
        finally:
            g2.close()
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


@test
def bad_data_pointer_contained(g):
    # The root directory's first data block points past blocks_count, inside a
    # drive made deliberately larger than the filesystem. Without a range check
    # that read would succeed and the crafted directory record below would make
    # /disk/hello.txt resolvable through a block the filesystem does not own.
    tmp, copy = _prepare()
    try:
        geo = _geometry(copy)
        bs = geo["block_size"]
        hello = _find_inode(copy, geo, 2, b"hello.txt")
        if hello is None:
            g.fail("the image has no /hello.txt to point at")

        # Grow the device past the filesystem, then write a valid ext2 dirent
        # for hello.txt in a block that lies inside the device but outside it.
        with open(copy, "ab") as f:
            f.write(b"\0" * (1024 * 1024))
        rogue = geo["blocks_count"] + 100
        if rogue * bs + bs > os.path.getsize(copy):
            g.fail("the test image is not large enough for the rogue block")
        record = bytearray(bs)
        struct.pack_into("<IHBB", record, 0, hello, bs, 9, 1)
        record[8:17] = b"hello.txt"
        with open(copy, "r+b") as f:
            f.seek(rogue * bs)
            f.write(record)

        _patch_u32(copy, _inode_offset(copy, geo, 2) + 40, rogue)  # i_block[0]

        g2 = Guest("hostile-pointer", disk_image=copy)
        try:
            g2.wait_for(r"\[Ext2\] [0-9]+-byte blocks")
            g2.wait_for(r"\[VFS\] no usable root directory on the disk")
            clean(g2)

            # The rogue block is inside the drive, so only the blocks_count
            # check stops the read. Its dirent must not resolve.
            mark = g2.mark()
            run(g2, "cat /disk/hello.txt")
            out = g2.since(mark)
            if "Hello from the ext2 disk" in out:
                g2.fail("a file was read through a block past blocks_count")
            clean(g2)
        finally:
            g2.close()
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    run_module(__file__)

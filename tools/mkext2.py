#!/usr/bin/env python3
"""Build the ext2 image the kernel mounts at /disk.

Made on the host with mke2fs, so the guest gets a real filesystem made by the
same tools a user would use, and we do not reimplement ext2 *creation*: the
kernel only has to read and update what mke2fs made.

The contents are fixed here rather than in files/ so the image is reproducible,
including a file deliberately larger than the twelve direct block pointers
(12 KB at 1 KB blocks): reading it exercises the indirect path, which is the
part of ext2 most likely to be quietly wrong. Two runs produce identical
bytes: the UUID and hash seed are constants, the staged tree is passed to
mke2fs as a tar archive with fixed ownership and timestamps, and
SOURCE_DATE_EPOCH stops the superblock carrying the wall clock.

    hello.txt      one line
    sub/nested.txt a file reached through a directory
    big.bin        20000 bytes of a pattern derived from the offset
    root/          an empty directory, the upper layer of the writable root

The guest mounts it read-write, but the RAM disk copies the image and the test
harness attaches the drive with snapshot=on, so the host image stays the
starting state every boot and a test can assert on the files above.

`root/` is where the kernel keeps the writable root: it mounts the same disk a
second time with that directory as the root inode and puts the overlay's upper
layer there. It ships empty so a fresh image has somewhere to write.
"""
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "iso_root", "disk.img")

BLOCKS = 16384       # 1 KB blocks -> a 16 MB image
INODES = 512         # 512 inodes, so a full disk is visible before it is full
BIG_SIZE = 20000     # > 12 KB, so the direct pointers are not enough

# Fixed image identity and timestamps. The UUID must be stable so two builds
# are comparable; the same value is used as the directory hash seed (ext2 has
# no dir_index here, but mke2fs otherwise fills the field from the clock).
IMAGE_UUID = "4d3c2b1a-0f9e-4d8c-9b7a-6e5d4c3b2a19"
IMAGE_EPOCH = "0"

# A name longer than 128 bytes. ext2 allows 255, and the point of NAME_MAX 256
# is that a file like this lists and opens instead of being silently cut down.
# Its contents are its own name, so a guest test can check what it read without
# the name being written down in two places.
LONG_NAME = "long-name-" + ("abcdefghij" * 19) + ".txt"   # 204 bytes


def big_byte(i):
    """Byte i of big.bin: printable, and a function of the offset so the guest
    can check every one without carrying the file around."""
    return 33 + (i % 94)


def main():
    tmp = tempfile.mkdtemp(prefix="gravitas-ext2-")
    stage = os.path.join(tmp, "stage")
    archive = os.path.join(tmp, "stage.tar")
    os.mkdir(stage)
    try:
        with open(os.path.join(stage, "hello.txt"), "w") as f:
            f.write("Hello from the ext2 disk\n")
        os.chmod(os.path.join(stage, "hello.txt"), 0o644)

        os.mkdir(os.path.join(stage, "sub"))
        os.chmod(os.path.join(stage, "sub"), 0o755)
        with open(os.path.join(stage, "sub", "nested.txt"), "w") as f:
            f.write("A nested file.\n")
        os.chmod(os.path.join(stage, "sub", "nested.txt"), 0o644)

        long_path = os.path.join(stage, "sub", LONG_NAME)
        with open(long_path, "w") as f:
            f.write(LONG_NAME)
        os.chmod(long_path, 0o644)

        with open(os.path.join(stage, "big.bin"), "w") as f:
            f.write("".join(chr(big_byte(i)) for i in range(BIG_SIZE)))
        os.chmod(os.path.join(stage, "big.bin"), 0o644)

        # The persistent root's backing directory. The kernel can create it if
        # an image predates it, but shipping it means a fresh disk already has
        # the directory the overlay's upper layer mounts.
        os.mkdir(os.path.join(stage, "root"))
        os.chmod(os.path.join(stage, "root"), 0o755)

        os.makedirs(os.path.dirname(OUT), exist_ok=True)
        if os.path.exists(OUT):
            os.unlink(OUT)

        # A tar archive, not the directory: mke2fs 1.47 and later restore the
        # ownership and timestamps recorded here, so the image does not depend
        # on which user (or which day) ran the build. Passing the directory
        # would stamp it with the build user's uid and the current time.
        subprocess.check_call([
            "tar", "--format=gnu", "--sort=name", "--mtime=@0",
            "--owner=0", "--group=0", "--numeric-owner",
            "-cf", archive, "-C", stage, ".",
        ])

        # ext2, no journal; dir_index off so directories stay a linear list
        # (the reader does not walk an htree). The image is the writable root's
        # upper layer as well as /disk, so it has to hold what a session writes:
        # a few copied images would exhaust the 128 inodes the first version
        # shipped, and a full disk should be a real condition to test rather
        # than the normal one.
        env = dict(os.environ, SOURCE_DATE_EPOCH=IMAGE_EPOCH)
        subprocess.check_call([
            "mke2fs", "-q", "-t", "ext2", "-b", "1024",
            "-O", "^dir_index,^resize_inode",
            "-N", str(INODES),
            "-U", IMAGE_UUID,
            "-E", "hash_seed=" + IMAGE_UUID,
            "-d", archive,
            "-F", "-L", "gravitas",
            OUT, str(BLOCKS),
        ], env=env)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("ext2 image: %s (%d KB)" % (os.path.relpath(OUT, ROOT), BLOCKS))
    return 0


if __name__ == "__main__":
    sys.exit(main())

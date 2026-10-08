#!/usr/bin/env python3
"""Process, IPC, console-endpoint, libc and filesystem contracts."""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from harness import test, run_module


@test
def background_spawn(g):
    # bg must return immediately, leaving the prompt usable while the child
    # runs; jobs/kill refer to it by pid.
    g.shell("bg sleeper.elf")
    g.wait_for(r"bg: started 'sleeper\.elf' as task \d+")
    g.shell("jobs")
    n = g.count("commands are programs")
    g.shell("help")
    g.wait_count("commands are programs", n + 1)   # the prompt stayed alive
    if not re.search(r"^\s+\d+\s+sleeper\.elf\s*$", g.after("bg: started"), re.M):
        g.fail("jobs did not list the background task while it ran")
    g.wait_for(r"\[sleeper\] done")


@test
def ctrl_c_interrupt(g):
    # A foreground program that never exits would block the shell forever. The
    # interrupt key kills the foreground job (the child the shell is blocked on)
    # and the prompt comes back.
    finished = g.count(r"\[shell\] program finished")
    g.shell("run hang.elf")
    g.wait_for("hang: running")
    g.ctrl("c")
    # Two paths: the scheduler was on another task, so kill_task ran; or it was
    # on the victim itself, so it exited through the zombie path.
    g.wait_for(r"Task 'hang\.elf' (terminated \(killed\)|exited)")
    g.wait_count(r"\[shell\] program finished", finished + 1)


@test
def process_control(g):
    g.shell("run proctest.elf")
    g.wait_for("all process-control checks pass")


@test
def ipc(g):
    g.shell("run msgtest.elf")
    g.wait_for("all ipc checks pass")


@test
def console_endpoint(g):
    # The child's output must be captured by its owner, NOT leak to the kernel
    # console.
    g.shell("run consoletest.elf")
    g.wait_for("consoletest: PASS")
    if "echo:ping" in g.log():
        g.fail("hosted program output leaked to the kernel console")


@test
def libc_basics(g):
    g.shell("run libctest.elf")
    g.wait_for("libctest: all checks pass")


@test
def file_api(g):
    g.shell("run filetest.elf")
    g.wait_for("filetest: all checks pass")


@test
def stdio(g):
    # The FILE* layer: buffered file I/O on the ramfs, fgets/fseek/ftell, the
    # console streams, and the ENOENT / EROFS error paths.
    g.shell("run stdiotest.elf")
    g.wait_for("stdiotest: all checks pass")


@test
def posix_facade(g):
    # The POSIX facade: open/read/write/close/lseek, stat, the console
    # descriptors, getcwd/chdir, and opendir/readdir/closedir.
    g.shell("run posixtest.elf")
    g.wait_for("posixtest: all checks pass")


@test
def overlay_root(g):
    # The writable root: writes land in an upper layer over the read-only image,
    # the listing is the union, and deleting an image file leaves a whiteout.
    g.shell("run overlaytest.elf")
    g.wait_for("overlaytest: all checks pass")


@test
def ext2_disk(g):
    # A real on-disk filesystem (ext2, made on the host) mounted read-only from
    # a RAM disk: metadata out of the inodes, the directory walk, and a file big
    # enough to need an indirect block.
    g.shell("run ext2test.elf")
    g.wait_for("ext2test: all checks pass")


@test
def path_limits(g):
    # A path or a name that does not fit is refused, never truncated: a
    # shortened path names a different file. Covers the depth limit, a 200-byte
    # ext2 name, and a name past NAME_MAX.
    g.shell("run pathtest.elf")
    g.wait_for("pathtest: all checks pass")


@test
def file_commands(g):
    # The userland file commands, driven the way a person would. This is the only
    # place mkdir's "the parent must exist", cp's chunked copy and rm -r's walk
    # are actually used.
    #
    # Every step waits for the program to *finish* rather than for one line of
    # its output: a listing is printed a line at a time, so waiting for one of
    # its lines says nothing about the rest. And the wait is anchored on that
    # program's own exit, not on the shell's generic "program finished" — a
    # background task from an earlier test can print that one late, and the wait
    # would then return before this command had run at all.
    def run(line):
        prog = line.split()[0]
        pat = r"Task '%s\.elf' exited" % prog
        n = g.count(pat)
        g.shell(line)
        g.wait_more(pat, n + 1)

    run("mkdir -p /tmp/fc/deep")
    run("touch /tmp/fc/a.txt")
    # The source is on the read-only ext2 mount, so no other test can have
    # changed it. (/hello.txt in the image *is* written by overlay_root, which
    # overwrites an image file on purpose and leaves it overwritten.)
    run("cp /disk/hello.txt /tmp/fc/b.txt")
    run("cat /tmp/fc/b.txt")
    g.wait_for("Hello from the ext2 disk")

    # The listing after the copy: the file cp made, the one touch made, and the
    # directory mkdir -p made.
    run("ls /tmp/fc")
    g.wait_for(r"^deep/$")          # the slash exists only in a listing
    if not g.count(r"^b\.txt$"):
        g.fail("cp produced nothing to list")

    # mv renames: the new name shows up, the old one is gone from every listing.
    run("mv /tmp/fc/b.txt /tmp/fc/c.txt")
    run("ls /tmp/fc")
    g.wait_for(r"^c\.txt$")

    # rm takes the file, rm -r takes the directory.
    c = g.count(r"^c\.txt$")
    d = g.count(r"^deep/$")
    run("rm /tmp/fc/c.txt")
    run("rm -r /tmp/fc/deep")
    run("ls /tmp/fc")
    after_c = g.count(r"^c\.txt$")
    after_d = g.count(r"^deep/$")
    if after_c != c:
        g.fail("rm left the file in the listing (%d, was %d)" % (after_c, c))
    if after_d != d:
        g.fail("rm -r left the directory in the listing (%d, was %d)" % (after_d, d))

    # mkdir without -p refuses a missing parent instead of inventing it, which is
    # the whole reason -p is a loop in the program rather than a mode in the
    # kernel.
    run("mkdir /tmp/fc/nope/deep")
    g.wait_for(r"mkdir: /tmp/fc/nope/deep: No such file")

    # ps reads the task list: the header, and itself running.
    run("ps")
    g.wait_for(r"^  PID STATE")
    g.wait_for(r"^\s+\d+ running\s+-\s+ps\.elf$")

    # df asks the filesystem how much room there is. The read-only mount reports
    # nothing free, and the overlay reports the ramfs cap it writes into — the
    # number that is otherwise invisible until a write is refused.
    run("df /disk")
    g.wait_for(r"^/disk\s+ext2\s+\d+K\s+0K\s+\(read-only\)$")
    run("df /")
    g.wait_for(r"^/\s+overlay\s+\d+K\s+\d+K\s*$")
    # A path that does not exist is an error, not the filesystem it would have
    # lived on.
    run("df /no/such/path")
    g.wait_for(r"df: /no/such/path: No such file")


@test
def shared_memory(g):
    # File mappings (zero-copy, out of the immutable image) and a shared region
    # that two tasks read and write through.
    g.shell("run shmtest.elf")
    g.wait_for("shmtest: all checks pass")


@test
def timer_wakeup(g):
    g.shell("run timertest.elf")
    g.wait_for("timertest: PASS")


if __name__ == "__main__":
    run_module(__file__)

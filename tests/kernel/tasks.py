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
def timer_wakeup(g):
    g.shell("run timertest.elf")
    g.wait_for("timertest: PASS")


if __name__ == "__main__":
    run_module(__file__)

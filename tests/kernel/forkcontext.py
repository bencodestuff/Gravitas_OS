#!/usr/bin/env python3
"""Fork: a child that returns out of the syscall it was copied from.

`forktest` drives the real scheduler and trap-return path in QEMU: the child
resumes after the call with rax = 0 on its own kernel stack, memory is private
after the copy-on-write split, descriptors stay shared, and the parent collects
the child status exactly once.

    python3 tests/kernel/forkcontext.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from harness import run_module, test


@test
def fork_returns_through_the_child(g):
    """The forked task resumes after the call, on its own kernel stack."""
    g.shell("run forktest.elf")
    g.wait_for(r"\[forktest\] (PASS|FAIL|[0-9]+ check)", timeout=120.0)
    out = g.after("[forktest]")
    if "FAIL" in out or "check(s) failed" in out:
        g.fail("forktest reported a failure:\n" + out[-1500:])
    g.wait_after("[forktest] PASS", "program finished", timeout=30.0)


if __name__ == "__main__":
    run_module(__file__)

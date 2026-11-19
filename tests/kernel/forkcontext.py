#!/usr/bin/env python3
"""A child that returns out of the syscall it was copied from.

The child-return hook is private: it exists only in an image built with
`configure.py --fork-test`, and `forktest` reports ENOSYS (and exits 0) when
the hook is absent. This group therefore skips quietly on an ordinary image and
does the real work on a fork-test one.

    python3 configure.py --fork-test && ninja iso
    python3 tests/kernel/forkcontext.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from harness import run_module, test


@test
def child_returns_through_its_own_stack(g):
    """The forked task resumes after the call, on its own kernel stack."""
    g.shell("run forktest.elf")
    g.wait_for(r"\[forktest\] (PASS|FAIL|ENOSYS|[0-9]+ check)", timeout=120.0)
    out = g.after("[forktest]")
    if "[forktest] ENOSYS" in out:
        # Ordinary image: there is no hook to drive, so there is nothing to
        # assert. Not a failure, just a build that does not carry the probe.
        return
    if "FAIL" in out or "check(s) failed" in out:
        g.fail("forktest reported a failure:\n" + out[-1500:])
    g.wait_after("[forktest] PASS", "program finished", timeout=30.0)


if __name__ == "__main__":
    run_module(__file__)

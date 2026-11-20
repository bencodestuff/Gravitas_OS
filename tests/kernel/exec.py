#!/usr/bin/env python3
"""Exec: a new image in the same process.

`exectest` forks twice. The first child execs itself and the parent's wait on
the id it forked must collect the new image's exit status, with the working
directory and an open descriptor carried across. The second child makes two
failing execs and has to come back to its old image intact, then exits with its
own code.

    python3 tests/kernel/exec.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from harness import run_module, test


@test
def exec_replaces_the_image_and_keeps_the_process(g):
    """A successful exec keeps the process; a failed one changes nothing."""
    g.shell("run exectest.elf")
    g.wait_for(r"\[exectest\] (PASS|FAIL)", timeout=120.0)
    out = g.after("[exectest]")
    if "FAIL" in out or "check(s) failed" in out:
        g.fail("exectest reported a failure:\n" + out[-2000:])
    g.wait_after("[exectest] PASS", "[shell] program finished", timeout=30.0)


if __name__ == "__main__":
    run_module(__file__)

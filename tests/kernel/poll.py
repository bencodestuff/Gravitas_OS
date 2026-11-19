#!/usr/bin/env python3
"""Readiness: POLL on pipes, without consuming them.

`polltest` asks the real kernel five times over: an empty pipe is not readable
and polling leaves its bytes in place; the deadline holds when nothing happens;
a write from another task ends the wait before the deadline; EOF and a broken
end are reported even when the caller asked for something else; and a
descriptor that is not there is named as such.

    python3 tests/kernel/poll.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from harness import run_module, test


@test
def pipe_readiness(g):
    """A poll tests a pipe and returns without taking anything out of it."""
    g.shell("run polltest.elf")
    g.wait_for(r"polltest: (all checks pass|[0-9]+ check\(s\) failed)", timeout=120.0)
    out = g.after("polltest:")
    if "FAIL" in out or "check(s) failed" in out:
        g.fail("polltest reported a failure:\n" + out[-1500:])
    g.wait_after("polltest: all checks pass", "program finished", timeout=30.0)


if __name__ == "__main__":
    run_module(__file__)

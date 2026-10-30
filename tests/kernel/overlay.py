#!/usr/bin/env python3
"""The writable root: copy-up, whiteouts, rename and long paths."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from harness import test, run_module


@test
def overlay_semantics(g):
    # The root is an overlay over the read-only image: writes copy up, the
    # listing is the union, a delete always leaves a marker, a rename of a
    # lower file removes the old name, long paths are carried whole, and
    # ".wh." is reserved for the markers themselves.
    g.shell("run overlaytest.elf")
    g.wait_for("overlaytest: all checks pass")


if __name__ == "__main__":
    run_module(__file__)

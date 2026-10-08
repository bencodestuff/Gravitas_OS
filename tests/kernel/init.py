#!/usr/bin/env python3
"""init: boot policy is a userland Lua manifest, not kernel code.

The kernel starts one program (init) and idles; init runs /init.lua, and that
script decides which programs start. These checks prove the manifest ran and
that it is an ordinary, readable file — the thing a user edits to add a program
to boot.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from harness import test, run_module


@test
def manifest_runs(g):
    # init prints its way through the manifest: load, then each service it
    # started. The task ids are whatever the kernel assigned.
    g.wait_for(r"\[init\] loading /init\.lua")
    g.wait_for(r"\[init\] starting userland")
    g.wait_for(r"\[init\] shell as task \d+")
    g.wait_for(r"\[init\] window manager as task \d+")
    g.wait_for(r"\[init\] terminal as task \d+")


@test
def init_is_resident(g):
    # init outlives the services it starts (the place a restart policy will
    # live), so it is still a task — parked, not gone.
    g.shell("run ps")
    g.wait_for(r"^\s+\d+\s+sleeping\s+-\s+init$")


@test
def manifest_is_a_file(g):
    # The manifest is a plain file in the writable root, so it can be read (and,
    # in the overlay, shadowed) without a rebuild. The service lines prove it is
    # the source the boot output above came from.
    g.shell("cat /init.lua")
    g.wait_for(r'init\.spawn\("shell\.elf"\)')
    g.wait_for(r'init\.spawn\("wm\.elf"\)')
    g.wait_for(r'init\.spawn\("term\.elf"\)')


if __name__ == "__main__":
    run_module(__file__)

#!/usr/bin/env python3
"""Leaving: the two ways out of the machine.

Both end the guest, so the group boots a second one for the second test. The
harness runs QEMU with -no-reboot, so a poweroff and a reset look the same from
here: the process exits.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from harness import Guest, test, run_module


@test
def power_off(g):
    g.shell("run poweroff.elf")
    g.wait_for("poweroff: shutting down")
    if not g.await_exit():
        g.fail("the guest did not power off")


@test
def reboot(g):
    # The first test's guest is gone, so this one boots its own.
    g2 = Guest("power-reboot")
    try:
        g2.shell("run poweroff.elf -r")
        g2.wait_for("poweroff: resetting")
        if not g2.await_exit():
            g2.fail("the guest did not reset")
    finally:
        g2.close()


if __name__ == "__main__":
    run_module(__file__)

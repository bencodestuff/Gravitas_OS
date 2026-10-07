#!/usr/bin/env python3
"""Fault contracts: the kernel kills the offending task, never the system."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from harness import test, run_module


@test
def crash_contract(g):
    # The symbol table covers the kernel and every program, so the dump names
    # the function a RIP is in — here the crashing program's own main.
    g.wait_for(r"\[Symbols\] self-check: kprint\+0x[0-9A-F]+")
    g.shell("run crash.elf")
    g.wait_for(r"FAULT: USERLAND in task 'crash\.elf'")
    g.wait_for(r"\[fault\] RIP=0x[0-9A-F]+ \(main\(.*\)\+0x[0-9A-F]+\)")
    g.wait_for("terminating task per the fault contract")
    g.wait_after("terminating task per the fault contract", "program finished")


@test
def stack_protector(g):
    g.shell("run smash.elf")
    g.wait_for("stack smashing detected")
    g.wait_after("stack smashing detected", "program finished")


@test
def debugger(g):
    # mon reads the introspection syscalls and the recorded crash. It runs
    # before stackoverflow so the recorded fault is still crash.elf.
    g.shell("run mon.elf")
    g.wait_for("mon: ticks")
    g.wait_for(r"mon: last fault: task 'crash\.elf'")


@test
def stack_guard(g):
    # Unbounded recursion hits the unmapped guard page below the user stack,
    # turning silent corruption into a named fault.
    g.shell("run stackoverflow.elf")
    g.wait_for("stack overflow detected")
    g.wait_for("terminating task per the fault contract")
    g.wait_after("terminating task per the fault contract", "program finished")


if __name__ == "__main__":
    run_module(__file__)

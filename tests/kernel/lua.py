#!/usr/bin/env python3
"""Lua: the third-party interpreter (third_party/lua-5.4.9, built to /bin/lua.elf).

Note the shell splits on spaces and has no quoting, so `-e` chunks must be
space-free; anything larger comes from a script in the image (/lua/hello.lua).
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from harness import test, run_module


@test
def lua_eval(g):
    # -e with a space-free chunk (the shell cannot quote).
    g.shell("run lua.elf -e print(2+3)")
    g.wait_for(r"^5$")


@test
def lua_script(g):
    # The whole standard library, from a script in the tar: strings, tables,
    # ipairs, math, os.date (the RTC), pcall, closures.
    g.shell("run lua.elf /lua/hello.lua")
    g.wait_for(r"hello from Lua 5\.4")
    g.wait_for(r"sum\t32")
    # 2^10 is a float in Lua 5.4: "%.14g" then ".0". This is the exact shape
    # that caught a printf %g bug (stale length after trimming zeros).
    g.wait_for(r"GRAVITAS\t8\t1024\.0\t3\.1415926535898")
    g.wait_for(r"squares:\t1 4 9 16 25")
    g.wait_for(r"pcall:\tfalse")
    # os.date reads the RTC: a plausible date this century.
    m = re.search(r"^20\d\d-\d\d-\d\d \d\d:\d\d:\d\d$", g.log(), re.M)
    if m is None:
        g.fail("os.date produced no plausible timestamp")


@test
def lua_error(g):
    # A runtime error is reported and the interpreter exits nonzero.
    g.shell("run lua.elf -e error('boom')")
    g.wait_for(r"boom")
    g.wait_for(r"\[shell\] program finished")


@test
def lua_writes_the_root(g):
    # The root is writable now: Lua creates a file in "/" (not /tmp) and reads
    # it back. `-e` chunks are space-free because the shell cannot quote.
    g.shell("run lua.elf -e io.open('/notes.txt','w'):write('hello-root')")
    g.wait_for(r"\[shell\] program finished")
    g.shell("run lua.elf -e print(io.open('/notes.txt'):read('a'))")
    g.wait_for(r"^hello-root$")
    g.shell("run lua.elf -e os.remove('/notes.txt')")
    g.wait_for(r"\[shell\] program finished")


@test
def lua_repl(g):
    # The interactive interpreter: banner, a typed expression, then Ctrl+C
    # (the foreground-job interrupt) ends it and the shell comes back.
    finished = g.count(r"\[shell\] program finished")
    g.shell("run lua.elf")
    g.wait_for(r"Lua 5\.4\.\d+  Copyright")
    g.shell("print(7*6)")
    g.wait_for(r"^42$")
    g.ctrl("c")
    g.wait_for(r"Task 'lua\.elf' (terminated \(killed\)|exited)")
    g.wait_count(r"\[shell\] program finished", finished + 1)


if __name__ == "__main__":
    run_module(__file__)

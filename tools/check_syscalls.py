#!/usr/bin/env python3
"""The syscall ABI's three descriptions must agree.

There are three hand-maintained lists of the same thing:

    src/libc/include/grv/syscall.h   GRV_SYSCALL_<NAME> <number>   (userland)
    src/arch/x86_64/idt.cpp          syscall_table[]               (the kernel)
    docs/syscalls.md                 the calls table               (the contract)

They are edited one number at a time, and this session added seven calls: the
kind of change where one of the three is forgotten. The checks, all in both
directions:

  * every documented number is a real `GRV_SYSCALL_*` with that number, and
    every constant is documented — so a doc name that no constant matches is a
    typo, not a convention;
  * every documented number has a dispatcher entry, and every entry is
    documented;
  * the dispatcher has no holes (a hole is a number nobody claimed, and the
    dispatcher answers it with ENOSYS).

The table's comments are kernel-side labels and legitimately differ from the
userland names (READ vs READFD), so they are not compared by name.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(ROOT, "src", "libc", "include", "grv", "syscall.h")
TABLE = os.path.join(ROOT, "src", "arch", "x86_64", "idt.cpp")
DOCS = os.path.join(ROOT, "docs", "syscalls.md")

# Documented as retired (the docs say so in prose): never claimed again, so they
# are the only legitimate holes.
RETIRED = {2, 3}


def from_header():
    """{number: NAME} from the userland constants."""
    out = {}
    for line in open(HEADER):
        m = re.match(r"#define\s+GRV_SYSCALL_([A-Z0-9_]+)\s+(\d+)\b", line)
        if m:
            out[int(m.group(2))] = m.group(1)
    return out


def from_table():
    """{number: function} from the dispatcher's table."""
    src = open(TABLE).read()
    start = src.index("syscall_table[]")
    body = src[start:src.index("};", start)]
    out = {}
    for m in re.finditer(r"/\*\s*(\d+)\s+[^*]*?\*/\s*(\w+)\s*,", body):
        out[int(m.group(1))] = m.group(2)
    return out


def from_docs():
    """{number: NAME} from the calls table."""
    out = {}
    for line in open(DOCS):
        m = re.match(r"\|\s*(\d+)\s*\|\s*`([A-Z0-9_]+)`", line)
        if m:
            out[int(m.group(1))] = m.group(2)
    return out


def main():
    header, table, docs = from_header(), from_table(), from_docs()
    bad = []

    for num, name in sorted(docs.items()):
        if num not in header:
            bad.append("docs: %d %s has no GRV_SYSCALL_%s in grv/syscall.h"
                       % (num, name, name))
        elif header[num] != name:
            bad.append("number %d: docs call it %s, the header %s"
                       % (num, name, header[num]))
        if num not in table:
            bad.append("docs: %d %s has no dispatcher entry" % (num, name))

    for num, name in sorted(header.items()):
        if num not in docs:
            bad.append("grv/syscall.h: GRV_SYSCALL_%s (%d) is not documented"
                       % (name, num))

    for num, fn in sorted(table.items()):
        # A `nullptr` slot is a deliberately retired number (the dispatcher
        # answers ENOSYS), which the docs describe in prose instead of a row.
        if num not in docs and fn != "nullptr":
            bad.append("dispatcher: %d (%s) is not documented" % (num, fn))

    if table:
        for num in range(min(table), max(table) + 1):
            if num not in table and num not in RETIRED:
                bad.append("dispatcher: number %d is a hole" % num)

    if bad:
        for b in bad:
            print("FAIL: " + b)
        return 1
    print("syscall header, dispatcher and docs agree (%d calls)" % len(table))
    return 0


if __name__ == "__main__":
    sys.exit(main())

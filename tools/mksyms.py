#!/usr/bin/env python3
"""Build the symbol table the fault handler names a RIP with.

Reads the kernel and every userland program, keeps their function symbols and
writes them sorted by address into a compact blob:

    "GSYM" | u32 count | count x {u64 addr, u32 size, u32 name offset} | names, NUL-separated

The kernel loads it at boot (it is a file in the tar) and binary-searches it.
It cannot be linked into the kernel: adding a table of symbols changes the very
addresses the table describes.

ELF parsing is done here in Python rather than by shelling out to nm, so the
build needs nothing beyond the toolchain it already uses. c++filt is used when
present, because a fault reading `_ZN3VFS8open_fdEPKci` is not much better than
one reading a bare address.
"""
import os
import struct
import subprocess
import sys

MAGIC = b"GSYM"
STT_FUNC = 2
SHN_UNDEF = 0


def read_symbols(path):
    """[(addr, name)] for the defined functions in one ELF64."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"\x7fELF" or data[4] != 2:  # ELF64
        return []

    e_shoff, = struct.unpack_from("<Q", data, 0x28)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    if e_shoff == 0 or e_shnum == 0:
        return []

    sections = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        name, stype, _flags, _addr, offset, size, link, _info, _align, entsize = \
            struct.unpack_from("<IIQQQQIIQQ", data, off)
        sections.append((name, stype, offset, size, link, entsize))

    out = []
    for _name, stype, offset, size, link, entsize in sections:
        if stype != 2:              # SHT_SYMTAB
            continue
        str_off, str_size = sections[link][2], sections[link][3]
        strtab = data[str_off:str_off + str_size]
        for pos in range(offset, offset + size, entsize):
            s_name, s_info, _other, s_shndx, s_value, s_size = \
                struct.unpack_from("<IBBHQQ", data, pos)
            if s_info & 0xF != STT_FUNC:      # functions only: a RIP is code
                continue
            if s_shndx == SHN_UNDEF or s_value == 0:
                continue
            end = strtab.find(b"\0", s_name)
            name = strtab[s_name:end].decode("utf-8", "replace")
            if not name or name.startswith("."):
                continue
            out.append((s_value, s_size, name))
    return out


def demangle(names):
    """Through c++filt when it exists; the mangled names otherwise."""
    try:
        p = subprocess.run(["c++filt"], input="\n".join(names), text=True,
                           capture_output=True, check=True)
        got = p.stdout.splitlines()
        return got if len(got) == len(names) else names
    except (OSError, subprocess.CalledProcessError):
        return names


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    out_path, inputs = argv[1], argv[2:]

    syms = []
    for path in inputs:
        if os.path.exists(path):
            syms += read_symbols(path)
    # Sort, and keep one name per address (aliases would just duplicate).
    syms.sort(key=lambda s: s[0])
    dedup = []
    for addr, size, name in syms:
        if dedup and dedup[-1][0] == addr:
            continue
        dedup.append((addr, size, name))

    names = demangle([n for _a, _s, n in dedup])
    blob = bytearray()
    offsets = []
    for n in names:
        offsets.append(len(blob))
        blob += n.encode("utf-8") + b"\0"

    with open(out_path, "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<I", len(dedup)))
        for (addr, size, _n), off in zip(dedup, offsets):
            f.write(struct.pack("<QII", addr, size, off))
        f.write(blob)

    print("symbols: %d functions from %d files -> %s (%d KB)"
          % (len(dedup), len(inputs), out_path,
             (len(blob) + 8 + len(dedup) * 16) // 1024))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

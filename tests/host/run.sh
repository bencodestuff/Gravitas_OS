#!/bin/sh
# Host unit tests — the libc pieces that build and run natively, with no QEMU
# boot. Most regressions are caught here in seconds, so this is the fast half
# of the gate (`ninja check`); the guest scenarios in tests/run.py are the
# slow half.
set -u

cd "$(dirname "$0")/../.." || exit 1

CC="${CC:-cc}"
ART="test_artifacts/host"
mkdir -p "$ART"

fail() { echo "FAIL: $*"; exit 1; }

# build_and_run <label> <binary> <source> [cc flags...]
#
# Options come after the source on purpose. Linkers that default to --as-needed
# (Ubuntu, and so CI) drop a library that appears before the object files that
# need it, so `-lm` ahead of the input left sqrt and friends unresolved. With
# the source first, a genuinely missing library fails on every host instead of
# only on the runner.
build_and_run() {
    label="$1"; bin="$2"; src="$3"; shift 3
    printf -- '-- %s\n' "$label"
    $CC -O2 "$src" $* -o "$ART/$bin" || fail "$label: compile failed"
    "$ART/$bin" || fail "$label: checks failed"
}

build_and_run "test_buffer (gravity text buffer)" \
    test_buffer tools/test_buffer.c -Isrc/libc/include
build_and_run "test_string (string/ctype/stdlib helpers)" \
    test_string tools/test_string.c -fno-builtin
build_and_run "test_stdio (FILE layer against a fake seam)" \
    test_stdio tools/test_stdio.c -fno-builtin -Isrc/libc/include
build_and_run "test_time (calendar + strftime)" \
    test_time tools/test_time.c -fno-builtin
build_and_run "test_printf (formatter)" \
    test_printf tools/test_printf.c -Isrc/libc/include \
        -ffunction-sections -fdata-sections -Wl,--gc-sections
build_and_run "test_libm_std (math + stdlib)" \
    test_libm_std tools/test_libm_std.c -fno-builtin -lm

# Not a C program: the page cache is C++, and it links against block.h alone so
# it can be driven over a fake sector device without booting the kernel.
printf -- '-- test_blockcache (page cache over a fake device)\n'
"${CXX:-c++}" -O2 -std=c++17 -Isrc -DBLOCKCACHE_HOST_TEST \
    tools/test_blockcache.cpp src/kernel/core/blockcache.cpp \
    -o "$ART/test_blockcache" || fail "test_blockcache: compile failed"
"$ART/test_blockcache" || fail "test_blockcache: checks failed"

# Not a C program: the path logic is the one piece of the filesystem with no
# kernel dependencies (no scheduler, console or VFS globals), so the table of
# cases runs here instead of needing a guest boot.
printf -- '-- test_path (the pure path rule)\n'
"${CXX:-c++}" -O2 -std=c++17 -Isrc \
    tools/test_path.cpp src/fs/path.cpp \
    -o "$ART/test_path" || fail "test_path: compile failed"
"$ART/test_path" || fail "test_path: checks failed"

# Not a C program: the ext2 write path's length rule is arithmetic, and the
# shipped image is far too small for a guest to reach the 4 GiB edge it guards.
# This drives the rule and the 32-bit store it replaced, which is what would
# wrap a file's recorded length back below the write.
printf -- '-- test_ext2_limits (the ext2 write path size rule)\n'
"${CXX:-c++}" -O2 -std=c++17 -Isrc \
    tools/test_ext2_limits.cpp \
    -o "$ART/test_ext2_limits" || fail "test_ext2_limits: compile failed"
"$ART/test_ext2_limits" || fail "test_ext2_limits: checks failed"

# Not a C program: tearing down an address space is a tree walk that frees
# pages, so it is driven over a stub frame manager that records every free. On
# a boot the real PMM silently ignores an already-free frame, which is what hid
# the double free this pins.
printf -- '-- test_vmm (address-space teardown)\n'
"${CXX:-c++}" -O2 -std=c++17 -DVMM_HOST_TEST -Isrc \
    tools/test_vmm.cpp src/kernel/core/vmm.cpp \
    -o "$ART/test_vmm" || fail "test_vmm: compile failed"
"$ART/test_vmm" || fail "test_vmm: checks failed"

# Not a C program: ext2 writes through the page cache, so a guest cannot make a
# rename's last directory removal fail: the block it just searched is a cache
# hit, and a cached write never reaches a device. The failure is injected inside
# write_block instead (EXT2_HOST_TEST, off in the kernel) and the rename state
# machine runs natively over an image tools/mkext2.py made, which is the same
# builder as the shipped disk. Without e2fsprogs the image cannot be made, and
# that skips this check rather than failing a machine with only a compiler.
printf -- '-- test_ext2_rename (a rename whose last removal fails)\n'
if python3 tools/mkext2.py "$ART/rename.img" >"$ART/rename.mkext2.log" 2>&1; then
    "${CXX:-c++}" -O2 -std=c++17 -Isrc -DEXT2_HOST_TEST -DBLOCKCACHE_HOST_TEST \
        tools/test_ext2_rename.cpp src/kernel/dev/ext2.cpp \
        src/kernel/core/blockcache.cpp \
        -o "$ART/test_ext2_rename" || fail "test_ext2_rename: compile failed"
    "$ART/test_ext2_rename" "$ART/rename.img" || fail "test_ext2_rename: checks failed"
else
    printf '   skipped: no ext2 image (e2fsprogs missing?)\n'
    tail -2 "$ART/rename.mkext2.log" | sed 's/^/   /'
fi

# Not a C program: the clean/dirty promise is about the order the medium sees
# writes in, which no guest can show (the page cache answers a read of a block
# it just wrote). The device here records every write and flush, so "the flag
# reached the medium before the data" is a fact about the bytes, and a dirty
# image's refusal is exercised without needing a second boot.
printf -- '-- test_ext2_clean_state (the dirty flag and a dirty mount)\n'
if python3 tools/mkext2.py "$ART/clean.img" >"$ART/clean.mkext2.log" 2>&1; then
    "${CXX:-c++}" -O2 -std=c++17 -Isrc -DEXT2_HOST_TEST -DBLOCKCACHE_HOST_TEST \
        tools/test_ext2_clean_state.cpp src/kernel/dev/ext2.cpp \
        src/kernel/core/blockcache.cpp \
        -o "$ART/test_ext2_clean_state" || fail "test_ext2_clean_state: compile failed"
    "$ART/test_ext2_clean_state" "$ART/clean.img" || fail "test_ext2_clean_state: checks failed"
else
    printf '   skipped: no ext2 image (e2fsprogs missing?)\n'
    tail -2 "$ART/clean.mkext2.log" | sed 's/^/   /'
fi

# Not a C program: the kernel and the libc describe the same syscall records in
# two headers that cannot include each other, so this is the only place they can
# be compared.
printf -- '-- test_abi (kernel vs ABI records)\n'
"${CXX:-c++}" -O2 -std=c++17 -Isrc -iquote src/libc/include tools/test_abi.cpp \
    -o "$ART/test_abi" || fail "test_abi: compile failed"
"$ART/test_abi" || fail "test_abi: the kernel and the ABI disagree"

# Not a C program: the syscall ABI is described in three places by hand, and
# this is the one that catches them drifting apart.
printf -- '-- check_syscalls (ABI vs docs)\n'
python3 tools/check_syscalls.py || fail "check_syscalls: the ABI and its docs disagree"

echo "HOST TESTS PASS"

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
build_and_run() {
    label="$1"; bin="$2"; src="$3"; shift 3
    printf -- '-- %s\n' "$label"
    $CC -O2 $* "$src" -o "$ART/$bin" || fail "$label: compile failed"
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

echo "HOST TESTS PASS"

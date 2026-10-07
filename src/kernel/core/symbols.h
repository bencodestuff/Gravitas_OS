#pragma once
#include <stdint.h>

// symbols — the function names behind a faulting RIP.
//
// The table is generated at build time (tools/mksyms.py) from the kernel and
// every userland program, and read from the tar at boot: it cannot be linked
// into the kernel, because adding it would change the very addresses it
// describes. Lookups cover both halves — a fault in a program's code names that
// program's function, and a fault in kernel code names the kernel's.

namespace Symbols {

// Take the blob (the caller keeps it alive; it is read once at boot). A bad or
// absent blob just means nothing resolves.
void init(const void* blob, uint64_t size);

// How many functions the table holds (0 when there is none).
uint32_t count();

// Write "name+0xNN" for the function containing 'addr'. Returns false — and
// writes nothing — when there is no table, or nothing covers the address (a
// bogus RIP should not be named after whatever function happens to precede it).
bool resolve(uint64_t addr, char* out, uint64_t max);

} // namespace Symbols

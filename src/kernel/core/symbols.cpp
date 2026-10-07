#include "kernel/core/symbols.h"

// See symbols.h. The blob is built by tools/mksyms.py:
//
//   "GSYM" | u32 count | count x {u64 addr, u32 size, u32 name offset} | names
//
// The entries are sorted by address, so a lookup is a binary search for the
// last one at or below the RIP — and then a check that the RIP is really inside
// it, which is what the size is for.

namespace Symbols {

struct Entry {
    uint64_t addr;
    uint32_t size;
    uint32_t name;
};

static const Entry* entries = nullptr;
static const char* names = nullptr;
static uint32_t entry_count = 0;

static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void init(const void* blob, uint64_t size) {
    entries = nullptr;
    names = nullptr;
    entry_count = 0;

    if (blob == nullptr || size < 8) return;
    const uint8_t* b = (const uint8_t*)blob;
    if (b[0] != 'G' || b[1] != 'S' || b[2] != 'Y' || b[3] != 'M') return;

    uint32_t n = rd32(b + 4);
    uint64_t need = 8 + (uint64_t)n * sizeof(Entry);
    if (need > size) return; // truncated: not usable

    entry_count = n;
    entries = (const Entry*)(b + 8);
    names = (const char*)(b + need);
}

uint32_t count() { return entry_count; }

bool resolve(uint64_t addr, char* out, uint64_t max) {
    if (entry_count == 0 || out == nullptr || max == 0) return false;

    // Binary search: the last entry whose start is <= addr.
    uint32_t lo = 0, hi = entry_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (entries[mid].addr <= addr) lo = mid + 1;
        else hi = mid;
    }
    if (lo == 0) return false;
    const Entry* e = &entries[lo - 1];

    uint64_t off = addr - e->addr;
    if (e->size != 0 && off >= e->size) return false; // in a gap: not this one

    // "name+0xNN"
    uint64_t n = 0;
    const char* src = names + e->name;
    while (*src && n + 1 < max) out[n++] = *src++;
    if (n + 5 >= max) { out[0] = '\0'; return false; }

    static const char hex[] = "0123456789ABCDEF";
    out[n++] = '+';
    out[n++] = '0';
    out[n++] = 'x';
    // The offset is small; print it without leading zeros.
    char digits[16];
    int d = 0;
    uint64_t v = off;
    if (v == 0) digits[d++] = '0';
    while (v > 0 && d < 16) { digits[d++] = hex[v & 0xF]; v >>= 4; }
    while (d > 0 && n + 1 < max) out[n++] = digits[--d];
    out[n] = '\0';
    return true;
}

} // namespace Symbols

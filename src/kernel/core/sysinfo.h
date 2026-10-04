#pragma once
#include <stdint.h>

// --- SysInfo: a read-only snapshot of system-wide counters ---
// The mechanism behind the userland monitor (`/bin/mon`). The layout is
// shared verbatim with libc's grv/syscall.h.

struct SysInfo {
    uint64_t ticks;
    uint64_t tasks;
    uint64_t mem_free;     // PMM free frames
    uint64_t mem_total;    // PMM total frames
    uint64_t heap_used;    // bytes
    uint64_t heap_peak;
    uint64_t heap_total;
    uint64_t surfaces;
    uint64_t comp_presents;
    uint64_t comp_full;
    uint64_t comp_rects;
    uint64_t comp_pixels;
};

namespace Sysinfo {
    void get(SysInfo* out);
}

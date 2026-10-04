#include "kernel/core/sysinfo.h"
#include "kernel/core/pmm.h"
#include "kernel/core/heap.h"
#include "kernel/core/timer.h"
#include "kernel/core/sched.h"
#include "kernel/gfx/compositor.h"

namespace Sysinfo {

void get(SysInfo* out) {
    if (out == nullptr) return;

    out->ticks = Timer::get_ticks();
    out->tasks = (uint64_t)Sched::task_count();

    out->mem_free = PMM::free_frames();
    out->mem_total = PMM::total_frames();

    out->heap_used = Heap::used();
    out->heap_peak = Heap::peak();
    out->heap_total = Heap::total();

    out->surfaces = (uint64_t)Compositor::surface_count();
    Compositor::Stats st = Compositor::stats();
    out->comp_presents = st.presents;
    out->comp_full = st.full_paints;
    out->comp_rects = st.rects;
    out->comp_pixels = st.pixels;

    out->screen_w = (uint64_t)Compositor::screen_width();
    out->screen_h = (uint64_t)Compositor::screen_height();
}

} // namespace Sysinfo

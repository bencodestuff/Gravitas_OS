#include "kernel/gfx/compositor_internal.h"
#include "kernel/klib/klib.h"
#include "kernel/core/sched.h"
#include "kernel/core/pmm.h"
#include "kernel/core/vmm.h"
#include "kernel/core/heap.h"
#include "kernel/core/ipc.h"

// --- Compositor: the surface registry and the lifecycle ---
//
// This unit owns the surface table, the draw order, the display authority and
// the measurement counters. A surface is a w*h 32bpp pixel buffer the kernel
// tracks but never interprets: physical frames (scattered — the PMM makes no
// contiguity promise), mapped into the owner task's address space, and read
// back through the HHDM by the painter. Z-order is an explicit bottom-to-top
// list; ownership is checked against the kernel-stamped task id on every op.
//
// The painting (damage, present, the cursor) is in compositor_paint.cpp and
// compositor_cursor.cpp; see compositor_internal.h for the seam.

namespace Compositor {

Surface surfaces[MAX_SURFACES];

// Draw order: bottom (drawn first) to top. Holds slot indices, so
// restacking never moves the Surface structs.
int order[MAX_SURFACES];
int order_count = 0;

int next_surface_id = 1;
int authority_task = -1; // display authority (see compositor.h)
int hotkey_mod_mask = 0; // modifier chord reserved by the authority

// --- registry helpers -----------------------------------------------------

int slot_of(const Surface* s) { return (int)(s - surfaces); }

Surface* find_owned(int id) {
    int me = Sched::current_task_id();
    for (int i = 0; i < MAX_SURFACES; i++) {
        if (surfaces[i].used && surfaces[i].id == id && surfaces[i].task_id == me)
            return &surfaces[i];
    }
    return nullptr;
}

// Like find_owned, but the display authority may control any surface (it
// places and restacks windows it does not own).
Surface* find_controllable(int id) {
    int me = Sched::current_task_id();
    bool auth = (authority_task != -1 && authority_task == me);
    for (int i = 0; i < MAX_SURFACES; i++) {
        if (surfaces[i].used && surfaces[i].id == id &&
            (surfaces[i].task_id == me || auth)) {
            return &surfaces[i];
        }
    }
    return nullptr;
}

void order_remove(int slot) {
    for (int i = 0; i < order_count; i++) {
        if (order[i] == slot) {
            for (int j = i; j < order_count - 1; j++) order[j] = order[j + 1];
            order_count--;
            return;
        }
    }
}

void order_insert(int slot, int pos) {
    if (pos < 0) pos = 0;
    if (pos > order_count) pos = order_count;
    for (int j = order_count; j > pos; j--) order[j] = order[j - 1];
    order[pos] = slot;
    order_count++;
}

// Release a registry slot without touching frames or page tables (callers
// differ on who owns those — see destroy vs. task teardown).
void release_slot(int slot) {
    order_remove(slot);
    surfaces[slot].used = false;
    surfaces[slot].frames = nullptr;
    surfaces[slot].page_count = 0;
}

void fill_info(const Surface* s, uint64_t z, Info* out) {
    out->address = s->va;
    out->width = s->w;
    out->height = s->h;
    out->id = (uint64_t)s->id;
    out->owner = (uint64_t)s->task_id;
    out->x = (int64_t)s->x;
    out->y = (int64_t)s->y;
    out->z = z;
}

// --- surface lifecycle ----------------------------------------------------

int surface_create(uint64_t w, uint64_t h) {
    if (!fb_ready()) return -1;
    if (w == 0 || h == 0) return -1;
    if (w > 32768 || h > 32768) return -1; // keeps w*h*4 provably in range

    uint64_t size = w * h * 4;
    if (size == 0 || size > MAX_SURFACE_BYTES) return -1;

    Surface* s = nullptr;
    int slot = -1;
    for (int i = 0; i < MAX_SURFACES; i++) {
        if (!surfaces[i].used) { s = &surfaces[i]; slot = i; break; }
    }
    if (s == nullptr) return -1;

    uint64_t pages = (size + 4095) / 4096;
    uint64_t* frames = (uint64_t*)Heap::malloc(pages * sizeof(uint64_t));
    if (frames == nullptr) return -1;

    uint64_t owner_cr3 = Sched::current_task_cr3();
    uint64_t old_cr3 = VMM::current_pml4();
    if (old_cr3 != owner_cr3) VMM::switch_pml4(owner_cr3);

    uint64_t va = VMM::alloc_user_range(owner_cr3, pages);
    if (va == 0) {
        if (old_cr3 != owner_cr3) VMM::switch_pml4(old_cr3);
        Heap::free(frames);
        return -1;
    }

    // Frames come one at a time and land wherever the PMM says — the
    // buffer is a scatter list, not a run.
    uint64_t got = 0;
    for (; got < pages; got++) {
        frames[got] = PMM::alloc_page();
        if (frames[got] == 0) break;
    }
    if (got < pages) {
        for (uint64_t p = 0; p < got; p++) PMM::free_page(frames[p]);
        VMM::unmap_user_range(owner_cr3, va, pages);
        if (old_cr3 != owner_cr3) VMM::switch_pml4(old_cr3);
        Heap::free(frames);
        return -1;
    }

    for (uint64_t p = 0; p < pages; p++) {
        VMM::map_page_in(owner_cr3, frames[p], va + p * 4096, 0x07); // P|W|U
    }
    if (old_cr3 != owner_cr3) VMM::switch_pml4(old_cr3);

    s->id = next_surface_id++;
    s->task_id = Sched::current_task_id();
    s->used = true;
    s->w = w; s->h = h;
    s->va = va;
    s->frames = frames;
    s->page_count = pages;
    s->x = 0; s->y = 0;

    // Zero the surface (fresh frames may hold boot garbage).
    for (uint64_t p = 0; p < pages; p++) {
        uint64_t left = size - p * 4096;
        memset(frame_ptr(frames[p]), 0, left < 4096 ? left : 4096);
    }

    order_insert(slot, order_count); // new surfaces start on top
    damage_surface(s);               // it now covers (0,0)-(w,h)

    // Tell the display authority a new window appeared (it owns policy).
    if (authority_task != -1 && s->task_id != authority_task)
        Ipc::post_kernel(authority_task, EVT_SURFACE_ADDED, (uint64_t)s->id, 0);

    return s->id;
}

// Resize an owned surface: allocate a buffer at the new size, release the old
// one, and repaint where both were. The id, position and z-order are kept; the
// pixels move, so the owner has to re-bind to the new VA.
int surface_resize(int id, uint64_t w, uint64_t h) {
    Surface* s = find_owned(id);
    if (s == nullptr) return -1;
    if (w == 0 || h == 0) return -1;
    if (w > 32768 || h > 32768) return -1;
    uint64_t size = w * h * 4;
    if (size == 0 || size > MAX_SURFACE_BYTES) return -1;
    if (w == s->w && h == s->h) return 0; // nothing to do

    uint64_t pages = (size + 4095) / 4096;
    uint64_t* frames = (uint64_t*)Heap::malloc(pages * sizeof(uint64_t));
    if (frames == nullptr) return -1;

    uint64_t owner_cr3 = Sched::current_task_cr3();
    uint64_t old_cr3 = VMM::current_pml4();
    if (old_cr3 != owner_cr3) VMM::switch_pml4(owner_cr3);

    // The new range is allocated before the old one is freed, so a failure
    // leaves the surface exactly as it was.
    uint64_t va = VMM::alloc_user_range(owner_cr3, pages);
    if (va == 0) {
        if (old_cr3 != owner_cr3) VMM::switch_pml4(old_cr3);
        Heap::free(frames);
        return -1;
    }

    uint64_t got = 0;
    for (; got < pages; got++) {
        frames[got] = PMM::alloc_page();
        if (frames[got] == 0) break;
    }
    if (got < pages) {
        for (uint64_t p = 0; p < got; p++) PMM::free_page(frames[p]);
        VMM::unmap_user_range(owner_cr3, va, pages);
        if (old_cr3 != owner_cr3) VMM::switch_pml4(old_cr3);
        Heap::free(frames);
        return -1;
    }

    for (uint64_t p = 0; p < pages; p++)
        VMM::map_page_in(owner_cr3, frames[p], va + p * 4096, 0x07); // P|W|U

    uint64_t old_va = s->va, old_pages = s->page_count, old_w = s->w, old_h = s->h;
    uint64_t* old_frames = s->frames;
    for (uint64_t p = 0; p < old_pages; p++) PMM::free_page(old_frames[p]);
    VMM::unmap_user_range(owner_cr3, old_va, old_pages);
    Heap::free(old_frames);
    if (old_cr3 != owner_cr3) VMM::switch_pml4(old_cr3);

    s->w = w; s->h = h;
    s->va = va;
    s->frames = frames;
    s->page_count = pages;

    for (uint64_t p = 0; p < pages; p++) {
        uint64_t left = size - p * 4096;
        memset(frame_ptr(frames[p]), 0, left < 4096 ? left : 4096);
    }

    damage_add(s->x, s->y, s->x + (int)old_w, s->y + (int)old_h);
    damage_surface(s);
    return 0;
}

int surface_destroy(int id) {    Surface* s = find_owned(id);
    if (s == nullptr) return -1;

    int slot = slot_of(s);
    int owner = s->task_id;
    uint64_t owner_cr3 = Sched::current_task_cr3(); // owner == the caller

    damage_surface(s); // repaint what it covered, after release
    for (uint64_t p = 0; p < s->page_count; p++) PMM::free_page(s->frames[p]);
    VMM::unmap_user_range(owner_cr3, s->va, s->page_count);
    Heap::free(s->frames);
    release_slot(slot);

    if (authority_task != -1 && owner != authority_task)
        Ipc::post_kernel(authority_task, EVT_SURFACE_REMOVED, (uint64_t)id, 0);

    return 0;
}

int surface_z(int id, int64_t pos) {
    Surface* s = find_controllable(id);
    if (s == nullptr) return -1;

    // Precise damage: changing z only changes the picture where the moved
    // surface overlaps another one (everywhere else it draws itself
    // exactly as before).
    for (int i = 0; i < MAX_SURFACES; i++) {
        if (!surfaces[i].used || &surfaces[i] == s) continue;
        Surface* t = &surfaces[i];
        int ix0 = imax(s->x, t->x);
        int iy0 = imax(s->y, t->y);
        int ix1 = imin(s->x + (int)s->w, t->x + (int)t->w);
        int iy1 = imin(s->y + (int)s->h, t->y + (int)t->h);
        if (ix1 > ix0 && iy1 > iy0) damage_add(ix0, iy0, ix1, iy1);
    }

    int slot = slot_of(s);
    order_remove(slot);
    if (pos < 0) order_insert(slot, 0);
    else if (pos >= (int64_t)order_count) order_insert(slot, order_count);
    else order_insert(slot, (int)pos);
    return 0;
}

void surface_move(int id, int x, int y) {
    Surface* s = find_controllable(id);
    if (s == nullptr) return;
    damage_surface(s);          // old position
    s->x = x;
    s->y = y;
    damage_surface(s);          // new position
}

bool surface_info(int id, Info* out) {
    for (int i = 0; i < order_count; i++) {
        Surface* s = &surfaces[order[i]];
        if (s->id == id) {
            fill_info(s, (uint64_t)i, out);
            return true;
        }
    }
    return false;
}

int surface_list(Info* out, uint64_t max) {
    if (out == nullptr) return -1;
    int n = 0;
    for (int i = 0; i < order_count && (uint64_t)n < max; i++) {
        fill_info(&surfaces[order[i]], (uint64_t)i, &out[n]);
        n++;
    }
    return n;
}

// --- display authority ---

bool claim_authority() {
    uint64_t flags = irq_save();
    int me = Sched::current_task_id();
    bool ok = (authority_task == -1 || authority_task == me);
    if (ok) authority_task = me;
    irq_restore(flags);
    return ok;
}

bool is_authority() {
    return authority_task != -1 && authority_task == Sched::current_task_id();
}

bool has_authority() { return authority_task != -1; }
int  authority_id() { return authority_task; }

void authority_gone(int task_id) {
    if (authority_task == task_id) {
        authority_task = -1;
        hotkey_mod_mask = 0; // the chord dies with its owner
    }
}

void set_hotkey_mask(int mask) { hotkey_mod_mask = mask; }
int  hotkey_mask() { return has_authority() ? hotkey_mod_mask : 0; }

int screen_width()  { fb_ready(); return (int)fb_w; }
int screen_height() { fb_ready(); return (int)fb_h; }

int cursor_x() { int x, y; cursor_pos(&x, &y); return x; }
int cursor_y() { int x, y; cursor_pos(&x, &y); return y; }

void set_console_on_top(bool on) {
    if (console_top == on) return;
    console_top = on;
    composite(); // the layering changed for the whole screen
}

void toggle_console_on_top() { set_console_on_top(!console_top); }

bool console_on_top() { return console_top; }

void surfaces_for_task_gone(int task_id) {
    for (int i = 0; i < MAX_SURFACES; i++) {
        if (surfaces[i].used && surfaces[i].task_id == task_id) {
            int sid = surfaces[i].id;
            damage_surface(&surfaces[i]); // repaint where it was
            // The frames and mappings die with the task's address space
            // (destroy_user_space frees them); only the registry entry and
            // the frame list are ours to release.
            Heap::free(surfaces[i].frames);
            release_slot(i);
            if (authority_task != -1 && task_id != authority_task)
                Ipc::post_kernel(authority_task, EVT_SURFACE_REMOVED, (uint64_t)sid, 0);
        }
    }
}

Stats stats() {
    Stats s;
    s.presents = stat_presents;
    s.full_paints = stat_full;
    s.rects = stat_rects;
    s.pixels = stat_pixels;
    return s;
}

int surface_count() {
    return order_count;
}

void dump_stats() {
    Stats s = stats();
    kprint("[dbg] compositor: ");
    kprint_uint(s.presents);    kprint(" presents, ");
    kprint_uint(s.full_paints); kprint(" full, ");
    kprint_uint(s.rects);       kprint(" rects, ");
    kprint_uint(s.pixels);      kprint(" px, dbuf ");
    kprint(back ? "on\n" : "off\n");
}

} // namespace Compositor

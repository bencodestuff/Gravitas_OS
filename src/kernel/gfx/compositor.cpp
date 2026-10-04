#include "kernel/gfx/compositor.h"
#include "kernel/klib/klib.h"
#include "kernel/dev/mouse.h"
#include "kernel/core/sched.h"
#include "kernel/core/pmm.h"
#include "kernel/core/vmm.h"
#include "kernel/core/heap.h"
#include "kernel/gfx/cursor.h"

extern volatile struct limine_framebuffer_request framebuffer_request;
extern volatile struct limine_hhdm_request hhdm_request;

// --- Compositor: the surface registry and the screen painter ---

namespace Compositor {

// Registry cap: slots are cheap (the frame list is heap-allocated), so
// this is really a guard against runaway clients, not a budget.
constexpr int MAX_SURFACES = 32;

// Sanity cap on a single surface (16M pixels at 32bpp ≈ 64MB is far past
// any screen); keeps w*h*4 well clear of 64-bit overflow.
constexpr uint64_t MAX_SURFACE_BYTES = 64ull << 20;

struct Surface {
    int id;
    int task_id;        // owner
    bool used;
    uint64_t w, h;
    uint64_t va;        // owner-space virtual address
    uint64_t* frames;   // heap array of physical frames (scattered)
    uint64_t page_count;
    int x, y;           // screen position (top-left)
};

static Surface surfaces[MAX_SURFACES];

// Draw order: bottom (drawn first) to top. Holds slot indices, so
// restacking never moves the Surface structs.
static int order[MAX_SURFACES];
static int order_count = 0;

static int next_surface_id = 1;
static bool dirty = true;   // recomposite on next commit if nothing pending

// --- the real framebuffer (kernel writes via HHDM) ---
static uint32_t* fb_base = nullptr;
static uint64_t fb_w = 0, fb_h = 0, fb_pitch_px = 0;

static bool fb_ready() {
    if (fb_base) return true;
    if (framebuffer_request.response == nullptr ||
        framebuffer_request.response->framebuffer_count < 1) return false;
    struct limine_framebuffer* fb = framebuffer_request.response->framebuffers[0];
    fb_base = (uint32_t*)(uintptr_t)fb->address;
    fb_w = fb->width;
    fb_h = fb->height;
    fb_pitch_px = fb->pitch / 4;
    return true;
}

static uint8_t* frame_ptr(uint64_t phys) {
    return (uint8_t*)(uintptr_t)(phys + hhdm_request.response->offset);
}

// --- registry helpers -----------------------------------------------------

static int slot_of(const Surface* s) { return (int)(s - surfaces); }

static Surface* find_owned(int id) {
    int me = Sched::current_task_id();
    for (int i = 0; i < MAX_SURFACES; i++) {
        if (surfaces[i].used && surfaces[i].id == id && surfaces[i].task_id == me)
            return &surfaces[i];
    }
    return nullptr;
}

static void order_remove(int slot) {
    for (int i = 0; i < order_count; i++) {
        if (order[i] == slot) {
            for (int j = i; j < order_count - 1; j++) order[j] = order[j + 1];
            order_count--;
            return;
        }
    }
}

static void order_insert(int slot, int pos) {
    if (pos < 0) pos = 0;
    if (pos > order_count) pos = order_count;
    for (int j = order_count; j > pos; j--) order[j] = order[j - 1];
    order[pos] = slot;
    order_count++;
}

// Release a registry slot without touching frames or page tables (callers
// differ on who owns those — see destroy vs. task teardown).
static void release_slot(int slot) {
    order_remove(slot);
    surfaces[slot].used = false;
    surfaces[slot].frames = nullptr;
    surfaces[slot].page_count = 0;
}

static void fill_info(const Surface* s, uint64_t z, Info* out) {
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
    dirty = true;
    return s->id;
}

int surface_destroy(int id) {
    Surface* s = find_owned(id);
    if (s == nullptr) return -1;

    int slot = slot_of(s);
    uint64_t owner_cr3 = Sched::current_task_cr3(); // owner == the caller

    for (uint64_t p = 0; p < s->page_count; p++) PMM::free_page(s->frames[p]);
    VMM::unmap_user_range(owner_cr3, s->va, s->page_count);
    Heap::free(s->frames);
    release_slot(slot);

    dirty = true;
    return 0;
}

int surface_z(int id, int64_t pos) {
    Surface* s = find_owned(id);
    if (s == nullptr) return -1;

    int slot = slot_of(s);
    order_remove(slot);
    if (pos < 0) order_insert(slot, 0);
    else if (pos >= (int64_t)order_count) order_insert(slot, order_count);
    else order_insert(slot, (int)pos);

    dirty = true;
    return 0;
}

void surface_move(int id, int x, int y) {
    Surface* s = find_owned(id);
    if (s == nullptr) return;
    s->x = x;
    s->y = y;
    dirty = true;
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

// --- painting -------------------------------------------------------------

void composite() {
    if (!fb_ready()) return;

    // Layer 0: surfaces bottom-to-top
    for (int oi = 0; oi < order_count; oi++) {
        Surface* s = &surfaces[order[oi]];

        // clip to screen
        int x0 = s->x < 0 ? 0 : s->x;
        int y0 = s->y < 0 ? 0 : s->y;
        int x1 = (s->x + (int)s->w > (int)fb_w) ? (int)fb_w : s->x + (int)s->w;
        int y1 = (s->y + (int)s->h > (int)fb_h) ? (int)fb_h : s->y + (int)s->h;
        if (x1 <= x0 || y1 <= y0) continue;

        for (int y = y0; y < y1; y++) {
            uint32_t* dst = fb_base + y * fb_pitch_px + x0;
            // Byte offset of this row's first visible pixel inside the
            // owner's (scattered) buffer. Pixel n lives in page
            // (offset>>12) at byte (offset & 0xFFF) — each pixel is
            // 4-byte aligned, so the read is always aligned.
            uint64_t off = ((uint64_t)y - s->y) * s->w * 4
                         + ((uint64_t)x0 - s->x) * 4;

            for (int x = x0; x < x1; x++) {
                uint32_t c = *(uint32_t*)(frame_ptr(s->frames[off >> 12]) + (off & 0xFFF));
                off += 4;

                // alpha blend: alpha in the top byte (0 = transparent)
                uint32_t a = c >> 24;
                if (a == 0) { dst++; continue; }              // fully transparent
                if (a == 0xFF) { *dst = c & 0x00FFFFFF; }     // opaque
                else {
                    uint32_t d = *dst;
                    uint32_t dr = d & 0xFF, dg = (d >> 8) & 0xFF, db = (d >> 16) & 0xFF;
                    uint32_t sr = c & 0xFF, sg = (c >> 8) & 0xFF, sb = (c >> 16) & 0xFF;
                    *dst = 0xFF000000
                         | (((dr + (sr - dr) * a / 255) & 0xFF))
                         | ((((dg + (sg - dg) * a / 255) & 0xFF)) << 8)
                         | ((((db + (sb - db) * a / 255) & 0xFF)) << 16);
                }
                dst++;
            }
        }
    }

    // Layer 1 (top): the mouse cursor
    int mx = Mouse::cursor_x(), my = Mouse::cursor_y();
    if (mx > (int)fb_w - CURSOR_W) mx = (int)fb_w - CURSOR_W;
    if (my > (int)fb_h - CURSOR_H) my = (int)fb_h - CURSOR_H;
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    Mouse::apply_position_clamp(mx, my);

    for (int cy = 0; cy < CURSOR_H; cy++) {
        uint32_t* dst = fb_base + (my + cy) * fb_pitch_px + mx;
        for (int cx = 0; cx < CURSOR_W; cx++) {
            uint8_t v = cursor_sprite16[cy][cx];
            if (v == 0) { dst++; continue; }
            *dst++ = (v == 1) ? 0x00000000 : 0x00FFFFFF;
        }
    }
}

void surfaces_for_task_gone(int task_id) {
    bool removed = false;
    for (int i = 0; i < MAX_SURFACES; i++) {
        if (surfaces[i].used && surfaces[i].task_id == task_id) {
            // The frames and mappings die with the task's address space
            // (destroy_user_space frees them); only the registry entry and
            // the frame list are ours to release.
            Heap::free(surfaces[i].frames);
            release_slot(i);
            removed = true;
        }
    }
    if (removed) dirty = true;
}

// called from surface_commit syscall
void composite_if_dirty(bool force) {
    if (dirty || force) composite();
    dirty = false;
}

} // namespace Compositor

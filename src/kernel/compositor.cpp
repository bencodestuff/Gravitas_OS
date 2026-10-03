#include "compositor.h"
#include "mouse.h"
#include "sched.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "../drivers/cursor.h"

extern volatile struct limine_framebuffer_request framebuffer_request;
extern volatile struct limine_hhdm_request hhdm_request;
extern "C" void kprint(const char* str);

// --- Basic compositor ---
// Z-order = creation order (later surfaces draw on top). Move-to-front
// and occlusion culling come when there's a reason to care.

namespace Compositor {

constexpr int MAX_SURFACES = 16;

struct Surface {
    int id;
    int task_id;        // owner
    bool used;
    uint64_t w, h;
    uint64_t va;        // owner-space virtual address
    uint64_t phys;      // physical base (contiguous pages) — HHDM-readable
    int x, y;           // screen position (top-left)
};

static Surface surfaces[MAX_SURFACES];
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

// --- 16x16 arrow cursor sprite lives in ../drivers/cursor.h ---
// (0 = transparent, 1 = black outline, 2 = white fill)

int surface_create(uint64_t w, uint64_t h) {
    if (!fb_ready()) return -1;

    Surface* s = nullptr;
    for (int i = 0; i < MAX_SURFACES; i++) {
        if (!surfaces[i].used) { s = &surfaces[i]; break; }
    }
    if (s == nullptr) return -1;

    uint64_t size = w * h * 4;
    uint64_t pages = (size + 4095) / 4096;

    // Physical pages: PMM hands out sequential frames, and we allocate
    // them back-to-back under the syscall's cli — so they're contiguous.
    // (This is what lets composite() read rows via one HHDM base.)
    uint64_t phys = PMM::alloc_page();
    if (phys == 0) return -1;
    for (uint64_t p = 1; p < pages; p++) {
        if (PMM::alloc_page() != phys + p * 4096) {
            // non-contiguous: leak them (rare); bail out — v1 keeps the
            // contiguous-only invariant rather than tracking scatter lists
            kprint("[Compositor] non-contiguous pages!\n");
            for (uint64_t q = 1; q < p; q++) PMM::free_page(phys + q * 4096);
            PMM::free_page(phys);
            return -1;
        }
    }

    // Map into the owner's space
    uint64_t owner_cr3 = Sched::current_task_cr3();
    uint64_t old_cr3 = VMM::current_pml4();
    if (old_cr3 != owner_cr3) VMM::switch_pml4(owner_cr3);
    uint64_t va = VMM::alloc_user_range(owner_cr3, pages);
    if (va == 0) {
        for (uint64_t p = 0; p < pages; p++) PMM::free_page(phys + p * 4096);
        if (old_cr3 != owner_cr3) VMM::switch_pml4(old_cr3);
        return -1;
    }
    for (uint64_t p = 0; p < pages; p++) {
        VMM::map_page_in(owner_cr3, phys + p * 4096, va + p * 4096, 0x07); // P|W|U
    }
    if (old_cr3 != owner_cr3) VMM::switch_pml4(old_cr3);

    s->id = next_surface_id++;
    s->task_id = Sched::current_task_id();
    s->used = true;
    s->w = w; s->h = h;
    s->va = va;
    s->phys = phys;
    s->x = 0; s->y = 0;

    // Zero the surface (fresh pages may contain boot garbage)
    uint8_t* hh = (uint8_t*)(uintptr_t)(phys + hhdm_request.response->offset);
    for (uint64_t i = 0; i < size; i++) hh[i] = 0;

    dirty = true;
    return s->id;
}

bool surface_info(int id, Info* out) {
    for (int i = 0; i < MAX_SURFACES; i++) {
        if (surfaces[i].used && surfaces[i].id == id) {
            out->address = surfaces[i].va;
            out->width = surfaces[i].w;
            out->height = surfaces[i].h;
            return true;
        }
    }
    return false;
}

void surface_move(int id, int x, int y) {
    for (int i = 0; i < MAX_SURFACES; i++) {
        if (surfaces[i].used && surfaces[i].id == id &&
            surfaces[i].task_id == Sched::current_task_id()) {
            surfaces[i].x = x;
            surfaces[i].y = y;
            dirty = true;
            return;
        }
    }
}

void composite() {
    if (!fb_ready()) return;

    // Layer 0: surfaces in creation order (later = on top)
    for (int i = 0; i < MAX_SURFACES; i++) {
        Surface* s = &surfaces[i];
        if (!s->used) continue;

        // clip to screen
        int x0 = s->x < 0 ? 0 : s->x;
        int y0 = s->y < 0 ? 0 : s->y;
        int x1 = (s->x + (int)s->w > (int)fb_w) ? (int)fb_w : s->x + (int)s->w;
        int y1 = (s->y + (int)s->h > (int)fb_h) ? (int)fb_h : s->y + (int)s->h;
        if (x1 <= x0 || y1 <= y0) continue;
        uint32_t* src = (uint32_t*)(uintptr_t)(s->phys + hhdm_request.response->offset);
        uint64_t src_pitch_px = (s->w * 4 + 3) / 4;

        for (int y = y0; y < y1; y++) {
            uint32_t* dst = fb_base + y * fb_pitch_px + x0;
            uint32_t* srow = src + (y - s->y) * src_pitch_px + (x0 - s->x);
            for (int x = x0; x < x1; x++) {
                uint32_t c = *srow++;
                // alpha blend: alpha in the top byte (0 = transparent)
                uint32_t a = c >> 24;
                if (a == 0) { continue; }        // fully transparent
                if (a == 0xFF) { *dst = c & 0x00FFFFFF; }
                else {
                    // per-channel blend
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
            // pages die with the task's address space (destroy_user_space
            // frees them) — just release the registry entry
            surfaces[i].used = false;
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
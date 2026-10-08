#pragma once
#include <stdint.h>
#include "kernel/gfx/compositor.h"

// Internal seam between the compositor's translation units. The public
// interface is compositor.h; nothing below is for callers outside the module.
//
// compositor.cpp holds the surface registry and lifecycle, the display
// authority and the measurement counters. compositor_paint.cpp owns the
// framebuffer, the damage list and the screen painter (composite/present and
// the surface commit path). compositor_cursor.cpp owns the pointer sprite.
//
// The registry and the painter share the surface table and the framebuffer
// geometry, so those live here as declarations and are defined once, in the
// unit named in the comment.

namespace Compositor {

// Registry cap: slots are cheap (the frame list is heap-allocated), so
// this is really a guard against runaway clients, not a budget.
constexpr int MAX_SURFACES = 32;

// Sanity cap on a single surface (16M pixels at 32bpp), keeping w*h*4
// well clear of 64-bit overflow.
constexpr uint64_t MAX_SURFACE_BYTES = 64ull << 20;

// Coalescing damage list. On overflow everything collapses to the
// bounding box — overdraw is always correct, underdraw never is.
constexpr int MAX_DAMAGE = 16;

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

struct Rect { int x0, y0, x1, y1; }; // x1/y1 exclusive

// --- shared state -------------------------------------------------------

// Surface registry (defined in compositor.cpp).
extern Surface surfaces[MAX_SURFACES];
extern int order[MAX_SURFACES];     // draw order, bottom to top
extern int order_count;
extern int next_surface_id;
extern int authority_task;          // display authority, or -1
extern int hotkey_mod_mask;         // modifier chord reserved by the authority

// The real framebuffer and the shadow buffer (defined in compositor_paint.cpp).
extern uint32_t* fb_base;
extern uint64_t fb_w, fb_h, fb_pitch_px;
extern uint32_t* back;              // shadow, heap-allocated on first use
extern uint32_t* g_paint;           // where the drawing primitives write

// Damage (defined in compositor_paint.cpp).
extern Rect damage[MAX_DAMAGE];
extern int damage_count;

// Paint counters (defined in compositor_paint.cpp).
extern uint64_t stat_presents, stat_full, stat_rects, stat_pixels;

// Console layer ordering (defined in compositor_paint.cpp).
extern bool console_top;

// Cursor state (defined in compositor_cursor.cpp).
extern int last_cursor_x, last_cursor_y;
extern bool cursor_seen;

// --- helpers ------------------------------------------------------------

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

// The paint and the damage list are single-consumer; keep out the timer
// (whose IRQ path also paints) while they run.
static inline uint64_t irq_save() {
    uint64_t flags;
    asm volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}
static inline void irq_restore(uint64_t flags) {
    asm volatile("pushq %0; popfq" :: "r"(flags) : "memory");
}

// --- framebuffer (compositor_paint.cpp) ---------------------------------

bool fb_ready();
void ensure_back();
void blit_rect(int x0, int y0, int x1, int y1);
uint8_t* frame_ptr(uint64_t phys);

// --- registry helpers (compositor.cpp) ----------------------------------

int slot_of(const Surface* s);
Surface* find_owned(int id);
Surface* find_controllable(int id);
void order_remove(int slot);
void order_insert(int slot, int pos);
void release_slot(int slot);
void fill_info(const Surface* s, uint64_t z, Info* out);

// --- damage (compositor_paint.cpp) --------------------------------------

void dmg_add_locked(int x0, int y0, int x1, int y1);
void damage_add(int x0, int y0, int x1, int y1);
void damage_surface(const Surface* s);

// --- painting (compositor_paint.cpp) ------------------------------------

void fill_rect(int x0, int y0, int x1, int y1, uint32_t color);
void paint_cell(int col, int row);
void stamp_surface(const Surface* s, int x0, int y0, int x1, int y1);
void paint_cells(int x0, int y0, int x1, int y1);
void stamp_surfaces(int x0, int y0, int x1, int y1);
void paint_rect(int x0, int y0, int x1, int y1,
                int* rx0, int* ry0, int* rx1, int* ry1);

// --- cursor (compositor_cursor.cpp) -------------------------------------

void cursor_pos(int* x, int* y);
void draw_cursor();

} // namespace Compositor

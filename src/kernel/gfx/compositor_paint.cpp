#include "kernel/gfx/compositor_internal.h"
#include "kernel/gfx/console.h"
#include "kernel/gfx/font.h"
#include "kernel/gfx/cursor.h"
#include "kernel/klib/klib.h"
#include "kernel/core/sched.h"
#include "kernel/core/heap.h"

#include <limine.h>

extern volatile struct limine_framebuffer_request framebuffer_request;
extern volatile struct limine_hhdm_request hhdm_request;

// --- Compositor: the screen painter ---
//
// This is the ONLY place that writes screen pixels, and it writes as few as
// possible: every mutation (surface op, console text, cursor motion) records a
// rectangle of damage, and present() repaints just those rectangles. A
// rectangle is repainted from scratch — background, then the console cells it
// covers, then every surface intersecting it — so z-order and "text under a
// window" fall out for free.
//
// Painting runs with interrupts disabled: a syscall enters through an
// interrupt gate, which clears IF, so no timer IRQ can preempt a paint or run
// the scheduler mid-frame.

namespace Compositor {

// --- the real framebuffer (kernel writes via HHDM) ---
uint32_t* fb_base = nullptr;
uint64_t fb_w = 0, fb_h = 0, fb_pitch_px = 0;

// Double buffering: paint_rect composes into a shadow buffer (the same
// geometry as the framebuffer), and present()/composite() blit the finished
// rectangles to the framebuffer in one tight copy. Without this the
// framebuffer briefly shows a rectangle's intermediate states — the
// background fill before the surfaces are stamped — which reads as flicker
// when a large window (e.g. the image viewer) moves.
uint32_t* back = nullptr;     // shadow, heap-allocated on first use
uint32_t* g_paint = nullptr;  // where the drawing primitives write

bool fb_ready() {
    if (fb_base) return true;
    if (framebuffer_request.response == nullptr ||
        framebuffer_request.response->framebuffer_count < 1) return false;
    struct limine_framebuffer* fb = framebuffer_request.response->framebuffers[0];
    fb_base = (uint32_t*)(uintptr_t)fb->address;
    fb_w = fb->width;
    fb_h = fb->height;
    fb_pitch_px = fb->pitch / 4;
    g_paint = fb_base;
    return true;
}

// Allocate the shadow once the heap is up. If it never fits we simply keep
// painting the framebuffer directly (correct, just without the buffer).
void ensure_back() {
    if (back) return;
    if (Heap::total() == 0) return; // heap not initialized yet
    back = (uint32_t*)Heap::malloc((size_t)fb_h * fb_pitch_px * sizeof(uint32_t));
}

// Copy a finished rectangle from the shadow to the framebuffer.
void blit_rect(int x0, int y0, int x1, int y1) {
    for (int y = y0; y < y1; y++) {
        uint32_t* s = back + (uint64_t)y * fb_pitch_px + x0;
        uint32_t* d = fb_base + (uint64_t)y * fb_pitch_px + x0;
        for (int x = x0; x < x1; x++) d[x - x0] = s[x - x0];
    }
}

uint8_t* frame_ptr(uint64_t phys) {
    return (uint8_t*)(uintptr_t)(phys + hhdm_request.response->offset);
}

// --- damage ----------------------------------------------------------------

Rect damage[MAX_DAMAGE];
int damage_count = 0;

uint64_t stat_presents = 0, stat_full = 0, stat_rects = 0, stat_pixels = 0;

void dmg_add_locked(int x0, int y0, int x1, int y1) {
    x0 = imax(x0, 0); y0 = imax(y0, 0);
    x1 = imin(x1, (int)fb_w); y1 = imin(y1, (int)fb_h);
    if (x1 <= x0 || y1 <= y0) return;

    // Merge into any rectangle it touches (touching counts).
    for (int i = 0; i < damage_count; i++) {
        Rect* d = &damage[i];
        if (x0 <= d->x1 && x1 >= d->x0 && y0 <= d->y1 && y1 >= d->y0) {
            d->x0 = imin(d->x0, x0); d->y0 = imin(d->y0, y0);
            d->x1 = imax(d->x1, x1); d->y1 = imax(d->y1, y1);
            return;
        }
    }
    if (damage_count < MAX_DAMAGE) {
        damage[damage_count].x0 = x0; damage[damage_count].y0 = y0;
        damage[damage_count].x1 = x1; damage[damage_count].y1 = y1;
        damage_count++;
        return;
    }
    // Full: collapse everything (and this rect) into one bounding box.
    for (int i = 0; i < damage_count; i++) {
        Rect* d = &damage[i];
        x0 = imin(x0, d->x0); y0 = imin(y0, d->y0);
        x1 = imax(x1, d->x1); y1 = imax(y1, d->y1);
    }
    damage[0].x0 = x0; damage[0].y0 = y0;
    damage[0].x1 = x1; damage[0].y1 = y1;
    damage_count = 1;
}

void damage_add(int x0, int y0, int x1, int y1) {
    uint64_t flags = irq_save();
    dmg_add_locked(x0, y0, x1, y1);
    irq_restore(flags);
}

void damage_surface(const Surface* s) {
    damage_add(s->x, s->y, s->x + (int)s->w, s->y + (int)s->h);
}

// --- painting -------------------------------------------------------------

void fill_rect(int x0, int y0, int x1, int y1, uint32_t color) {
    for (int y = y0; y < y1; y++) {
        uint32_t* row = g_paint + (uint64_t)y * fb_pitch_px;
        for (int x = x0; x < x1; x++) row[x] = color;
    }
}

void paint_cell(int col, int row) {
    int x = col * Font::cell_w();
    int y = row * Font::cell_h();
    Font::draw_char(x, y, Console::cell(col, row),
                    Console::cell_fg(col, row), Console::cell_bg(col, row));
}

// Alpha-blend a clipped screen rectangle of a surface into the framebuffer.
// [x0,x1) x [y0,y1) must already be inside the screen and the surface.
void stamp_surface(const Surface* s, int x0, int y0, int x1, int y1) {
    for (int y = y0; y < y1; y++) {
        uint32_t* dst = g_paint + (uint64_t)y * fb_pitch_px + x0;
        // Byte offset of this row's first visible pixel inside the owner's
        // (scattered) buffer: pixel n lives in page (offset>>12) at byte
        // (offset & 0xFFF).
        uint64_t off = ((uint64_t)y - s->y) * s->w * 4 + ((uint64_t)x0 - s->x) * 4;
        for (int x = x0; x < x1; x++) {
            uint32_t c = *(uint32_t*)(frame_ptr(s->frames[off >> 12]) + (off & 0xFFF));
            off += 4;

            uint32_t a = c >> 24;
            if (a == 0) { dst++; continue; }            // transparent
            if (a == 0xFF) { *dst = c & 0x00FFFFFF; }   // opaque
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

// Repaint one screen rectangle from scratch: background, then the console
// cells it covers, then every surface intersecting it, bottom-to-top.
// When the debug overlay is on, the cells are drawn AFTER the surfaces so
// kernel text (faults, dumps, the monitor) stays visible over windows.
bool console_top = false;

void paint_cells(int x0, int y0, int x1, int y1) {
    int cols = Console::cols(), rows = Console::rows();
    int cw = Font::cell_w(), ch = Font::cell_h();
    if (cols <= 0 || rows <= 0) return;
    int c0 = imax(x0 / cw, 0), c1 = imin((x1 - 1) / cw, cols - 1);
    int r0 = imax(y0 / ch, 0), r1 = imin((y1 - 1) / ch, rows - 1);
    for (int r = r0; r <= r1; r++)
        for (int c = c0; c <= c1; c++)
            paint_cell(c, r);
}

void stamp_surfaces(int x0, int y0, int x1, int y1) {
    for (int oi = 0; oi < order_count; oi++) {
        Surface* s = &surfaces[order[oi]];
        int sx0 = imax(x0, imax(s->x, 0));
        int sy0 = imax(y0, imax(s->y, 0));
        int sx1 = imin(x1, imin(s->x + (int)s->w, (int)fb_w));
        int sy1 = imin(y1, imin(s->y + (int)s->h, (int)fb_h));
        if (sx1 > sx0 && sy1 > sy0) stamp_surface(s, sx0, sy0, sx1, sy1);
    }
}

void paint_rect(int x0, int y0, int x1, int y1,
                int* rx0, int* ry0, int* rx1, int* ry1) {
    *rx0 = *rx1 = 0; *ry0 = *ry1 = 0;
    x0 = imax(x0, 0); y0 = imax(y0, 0);
    x1 = imin(x1, (int)fb_w); y1 = imin(y1, (int)fb_h);
    if (x1 <= x0 || y1 <= y0) return;

    // Snap outward to cell boundaries. paint_cells() draws WHOLE cells, and
    // a cell that merely intersects the rect extends past it — so the
    // background fill and the surface stamp below must cover that same
    // expanded area. Otherwise cell pixels leak past the rect and paint the
    // console on top of a surface: the cursor-repair rects are not
    // cell-aligned, so moving the pointer over a window leaves a trail of
    // console-coloured blocks.
    int cw = Font::cell_w(), ch = Font::cell_h();
    x0 = (x0 / cw) * cw;
    y0 = (y0 / ch) * ch;
    x1 = imin(((x1 + cw - 1) / cw) * cw, (int)fb_w);
    y1 = imin(((y1 + ch - 1) / ch) * ch, (int)fb_h);
    if (x1 <= x0 || y1 <= y0) return;

    fill_rect(x0, y0, x1, y1, Console::background());
    if (console_top) {
        stamp_surfaces(x0, y0, x1, y1);
        paint_cells(x0, y0, x1, y1);
    } else {
        paint_cells(x0, y0, x1, y1);
        stamp_surfaces(x0, y0, x1, y1);
    }

    *rx0 = x0; *ry0 = y0; *rx1 = x1; *ry1 = y1;
    stat_rects++;
    stat_pixels += (uint64_t)(x1 - x0) * (uint64_t)(y1 - y0);
}

// --- the two entry points -------------------------------------------------

void composite() {
    if (!fb_ready()) return;
    uint64_t flags = irq_save();

    ensure_back();
    g_paint = back ? back : fb_base;
    if (back) Font::set_target(back, fb_pitch_px, (int)fb_w, (int)fb_h);

    int rx0, ry0, rx1, ry1;
    paint_rect(0, 0, (int)fb_w, (int)fb_h, &rx0, &ry0, &rx1, &ry1);
    damage_count = 0;
    draw_cursor();

    if (back) {
        if (rx1 > rx0 && ry1 > ry0) blit_rect(rx0, ry0, rx1, ry1);
        Font::set_target(nullptr, 0, 0, 0);
    }
    g_paint = fb_base;
    stat_full++;
    irq_restore(flags);
}

void present() {
    if (!fb_ready()) return;

    uint64_t flags = irq_save();

    // Console text: dirty cell rows become damage rectangles.
    int cw = Font::cell_w(), ch = Font::cell_h();
    int rows = Console::rows();
    for (int r = 0; r < rows; r++) {
        int c0, c1;
        if (!Console::row_dirty(r, &c0, &c1)) continue;
        dmg_add_locked(c0 * cw, r * ch, (c1 + 1) * cw, (r + 1) * ch);
        Console::clean_row(r);
    }

    // Cursor motion: damage where it was and where it is now.
    int cx, cy;
    cursor_pos(&cx, &cy);
    bool moved = (cx != last_cursor_x || cy != last_cursor_y);
    if (moved) {
        dmg_add_locked(last_cursor_x, last_cursor_y,
                       last_cursor_x + CURSOR_W, last_cursor_y + CURSOR_H);
        dmg_add_locked(cx, cy, cx + CURSOR_W, cy + CURSOR_H);
    }

    if (damage_count == 0 && !moved && cursor_seen) {
        irq_restore(flags);
        return; // nothing to do — the common idle tick
    }

    ensure_back();
    g_paint = back ? back : fb_base;
    if (back) Font::set_target(back, fb_pitch_px, (int)fb_w, (int)fb_h);

    int n = 0;
    int rx0[MAX_DAMAGE], ry0[MAX_DAMAGE], rx1[MAX_DAMAGE], ry1[MAX_DAMAGE];
    for (int i = 0; i < damage_count; i++) {
        int a, b, c, d;
        paint_rect(damage[i].x0, damage[i].y0, damage[i].x1, damage[i].y1,
                   &a, &b, &c, &d);
        if (c > a && d > b) { rx0[n] = a; ry0[n] = b; rx1[n] = c; ry1[n] = d; n++; }
    }
    damage_count = 0;

    draw_cursor(); // always last: a painted rect may have covered it

    if (back) {
        for (int i = 0; i < n; i++) blit_rect(rx0[i], ry0[i], rx1[i], ry1[i]);
        Font::set_target(nullptr, 0, 0, 0);
    }
    g_paint = fb_base;
    stat_presents++;
    irq_restore(flags);
}

void surface_commit(int id, int rx, int ry, int rw, int rh) {
    int me = Sched::current_task_id();
    bool whole = (rw <= 0 || rh <= 0);
    for (int i = 0; i < MAX_SURFACES; i++) {
        if (!surfaces[i].used || surfaces[i].task_id != me) continue;
        if (id != 0 && surfaces[i].id != id) continue; // commit one surface
        if (whole) {
            damage_surface(&surfaces[i]);
        } else {
            damage_add(surfaces[i].x + rx, surfaces[i].y + ry,
                       surfaces[i].x + rx + rw, surfaces[i].y + ry + rh);
        }
    }
    present();
}

} // namespace Compositor

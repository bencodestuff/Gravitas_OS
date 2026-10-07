#include "kernel/gfx/compositor.h"
#include "kernel/gfx/console.h"
#include "kernel/gfx/font.h"
#include "kernel/klib/klib.h"
#include "kernel/dev/mouse.h"
#include "kernel/core/sched.h"
#include "kernel/core/pmm.h"
#include "kernel/core/vmm.h"
#include "kernel/core/heap.h"
#include "kernel/core/ipc.h"
#include "kernel/gfx/cursor.h"

extern volatile struct limine_framebuffer_request framebuffer_request;
extern volatile struct limine_hhdm_request hhdm_request;

// --- Compositor: the surface registry and the screen painter ---
//
// This is the ONLY module that writes screen pixels, and it writes as few
// as possible: every mutation (surface op, console text, cursor motion)
// records a rectangle of damage, and present() repaints just those
// rectangles. A rectangle is repainted from scratch — background, then
// the console cells it covers, then every surface intersecting it — so
// z-order and "text under a window" fall out for free.
//
// Painting runs with interrupts disabled: syscalls enter through a trap
// gate (IF stays on), so a timer IRQ could otherwise interrupt a paint
// or run the scheduler mid-frame.

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

static Surface surfaces[MAX_SURFACES];

// Draw order: bottom (drawn first) to top. Holds slot indices, so
// restacking never moves the Surface structs.
static int order[MAX_SURFACES];
static int order_count = 0;

static int next_surface_id = 1;
static int authority_task = -1; // display authority (see compositor.h)
static int hotkey_mod_mask = 0; // modifier chord reserved by the authority

// --- the real framebuffer (kernel writes via HHDM) ---
static uint32_t* fb_base = nullptr;
static uint64_t fb_w = 0, fb_h = 0, fb_pitch_px = 0;

// Double buffering: paint_rect composes into a shadow buffer (the same
// geometry as the framebuffer), and present()/composite() blit the finished
// rectangles to the framebuffer in one tight copy. Without this the
// framebuffer briefly shows a rectangle's intermediate states — the
// background fill before the surfaces are stamped — which reads as flicker
// when a large window (e.g. the image viewer) moves.
static uint32_t* back = nullptr;     // shadow, heap-allocated on first use
static uint32_t* g_paint = nullptr;  // where the drawing primitives write

static bool fb_ready() {
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
static void ensure_back() {
    if (back) return;
    if (Heap::total() == 0) return; // heap not initialized yet
    back = (uint32_t*)Heap::malloc((size_t)fb_h * fb_pitch_px * sizeof(uint32_t));
}

// Copy a finished rectangle from the shadow to the framebuffer.
static void blit_rect(int x0, int y0, int x1, int y1) {
    for (int y = y0; y < y1; y++) {
        uint32_t* s = back + (uint64_t)y * fb_pitch_px + x0;
        uint32_t* d = fb_base + (uint64_t)y * fb_pitch_px + x0;
        for (int x = x0; x < x1; x++) d[x - x0] = s[x - x0];
    }
}

static uint8_t* frame_ptr(uint64_t phys) {
    return (uint8_t*)(uintptr_t)(phys + hhdm_request.response->offset);
}

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

// --- interrupt-state guard ---
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

// --- damage ----------------------------------------------------------------

struct Rect { int x0, y0, x1, y1; }; // x1/y1 exclusive

static Rect damage[MAX_DAMAGE];
static int damage_count = 0;

static uint64_t stat_presents = 0, stat_full = 0, stat_rects = 0, stat_pixels = 0;

static void dmg_add_locked(int x0, int y0, int x1, int y1) {
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

static void damage_add(int x0, int y0, int x1, int y1) {
    uint64_t flags = irq_save();
    dmg_add_locked(x0, y0, x1, y1);
    irq_restore(flags);
}

static void damage_surface(const Surface* s) {
    damage_add(s->x, s->y, s->x + (int)s->w, s->y + (int)s->h);
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

// Like find_owned, but the display authority may control any surface (it
// places and restacks windows it does not own).
static Surface* find_controllable(int id) {
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

// --- painting -------------------------------------------------------------

static void fill_rect(int x0, int y0, int x1, int y1, uint32_t color) {
    for (int y = y0; y < y1; y++) {
        uint32_t* row = g_paint + (uint64_t)y * fb_pitch_px;
        for (int x = x0; x < x1; x++) row[x] = color;
    }
}

static void paint_cell(int col, int row) {
    int x = col * Font::cell_w();
    int y = row * Font::cell_h();
    Font::draw_char(x, y, Console::cell(col, row),
                    Console::cell_fg(col, row), Console::cell_bg(col, row));
}

// Alpha-blend a clipped screen rectangle of a surface into the framebuffer.
// [x0,x1) x [y0,y1) must already be inside the screen and the surface.
static void stamp_surface(const Surface* s, int x0, int y0, int x1, int y1) {
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
static bool console_top = false;

static void paint_cells(int x0, int y0, int x1, int y1) {
    int cols = Console::cols(), rows = Console::rows();
    int cw = Font::cell_w(), ch = Font::cell_h();
    if (cols <= 0 || rows <= 0) return;
    int c0 = imax(x0 / cw, 0), c1 = imin((x1 - 1) / cw, cols - 1);
    int r0 = imax(y0 / ch, 0), r1 = imin((y1 - 1) / ch, rows - 1);
    for (int r = r0; r <= r1; r++)
        for (int c = c0; c <= c1; c++)
            paint_cell(c, r);
}

static void stamp_surfaces(int x0, int y0, int x1, int y1) {
    for (int oi = 0; oi < order_count; oi++) {
        Surface* s = &surfaces[order[oi]];
        int sx0 = imax(x0, imax(s->x, 0));
        int sy0 = imax(y0, imax(s->y, 0));
        int sx1 = imin(x1, imin(s->x + (int)s->w, (int)fb_w));
        int sy1 = imin(y1, imin(s->y + (int)s->h, (int)fb_h));
        if (sx1 > sx0 && sy1 > sy0) stamp_surface(s, sx0, sy0, sx1, sy1);
    }
}

static void paint_rect(int x0, int y0, int x1, int y1,
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

// --- cursor ---------------------------------------------------------------

static int last_cursor_x = -10000, last_cursor_y = -10000;
static bool cursor_seen = false;

static void cursor_pos(int* x, int* y) {
    int mx = Mouse::cursor_x(), my = Mouse::cursor_y();
    if (mx > (int)fb_w - CURSOR_W) mx = (int)fb_w - CURSOR_W;
    if (my > (int)fb_h - CURSOR_H) my = (int)fb_h - CURSOR_H;
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    Mouse::apply_position_clamp(mx, my);
    *x = mx; *y = my;
}

static void draw_cursor() {
    int mx, my;
    cursor_pos(&mx, &my);
    for (int cy = 0; cy < CURSOR_H; cy++) {
        uint32_t* dst = g_paint + (uint64_t)(my + cy) * fb_pitch_px + mx;
        for (int cx = 0; cx < CURSOR_W; cx++) {
            uint8_t v = cursor_sprite16[cy][cx];
            if (v == 0) { dst++; continue; }
            *dst++ = (v == 1) ? 0x00000000 : 0x00FFFFFF;
        }
    }
    last_cursor_x = mx;
    last_cursor_y = my;
    cursor_seen = true;
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

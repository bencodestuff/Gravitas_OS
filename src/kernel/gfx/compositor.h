#pragma once
#include <stdint.h>

// --- Compositor: the surface registry and the screen painter ---
//
// A surface is a w*h 32bpp pixel buffer the kernel tracks but never
// interprets: physical frames (scattered — the PMM makes no contiguity
// promise), mapped into the owner task's address space, and read back by
// the kernel through the HHDM so compositing works whichever task's CR3
// is active. The kernel knows geometry, ownership and draw order; it has
// no idea what a "window" is.
//
// Z-ORDER is an explicit bottom-to-top draw list (creation order seeds
// it). surface_z re-inserts one surface at a clamped position; every
// surface also carries screen position, and compositing clips to the
// framebuffer.
//
// LIFETIME: every op except surface_list is owner-checked against the
// kernel-stamped task id (userland cannot address another task's
// surface). A surface dies explicitly via surface_destroy — freeing its
// frames and unmapping its VA — or implicitly with its owner
// (surfaces_for_task_gone, where the address space teardown frees the
// frames).

namespace Compositor {

    // Create a w*h 32bpp surface in the calling task's address space.
    // Returns the surface id, or -1 (bad size, no frames, no VA, full
    // registry). Pixels are at Info.address (owner-space VA).
    int surface_create(uint64_t w, uint64_t h);

    // Position a surface (top-left corner, may be negative). Clipped at
    // composite time. Owner-checked; silent no-op otherwise.
    void surface_move(int id, int x, int y);

    // Destroy an owned surface: free its frames, drop its mappings and
    // registry entry. 0 on success, -1 if unknown or not ours.
    int surface_destroy(int id);

    // Restack an owned surface at draw position 'pos' (0 = bottom, count-1
    // = top). Negative means bottom, huge means top, so callers needn't
    // know the surface count. 0 on success, -1 if unknown or not ours.
    int surface_z(int id, int64_t pos);

    // A surface's userland-visible record. 'address' is only meaningful
    // in the OWNER's address space; everything else is public geometry.
    struct Info {
        uint64_t address;  // owner-space VA
        uint64_t width;
        uint64_t height;
        uint64_t id;
        uint64_t owner;    // owning task id
        int64_t  x;        // screen position (top-left)
        int64_t  y;
        uint64_t z;        // draw position, 0 = bottom
    };

    // Single-surface lookup by id (used by the create path). Any task may
    // read geometry — 'address' is blurred to non-owners by the MMU.
    bool surface_info(int id, Info* out);

    // Copy up to 'max' records, bottom-to-top (index = z). Returns the
    // number written, or -1 for a null buffer. Read-only; any task may
    // enumerate (owner filtering is userland policy).
    int surface_list(Info* out, uint64_t max);

    // Full-screen repaint (boot, font swap). Rare; the normal path is
    // present().
    void composite();

    // Flush all pending damage: console text rows, surface changes and
    // cursor motion, repainting only the affected rectangles. Cheap when
    // little changed, a no-op when nothing did, and the only place pixels
    // are painted.
    void present();

    // A surface commit: damage every surface owned by the calling task and
    // flush. When rw > 0 and rh > 0, only the surface-local rectangle
    // (rx,ry,rw,rh) is damaged — how a full-screen overlay commits cheaply.
    void surface_commit(int rx, int ry, int rw, int rh);

    // --- Display authority (mechanism) ---
    // One task may claim authority: it can move/restack any surface, own the
    // mouse, and set keyboard focus — the mechanisms a window manager needs.
    // The kernel still knows nothing about "windows"; this is control over
    // surfaces, not window semantics.
    bool claim_authority();           // true if the caller is (now) the authority
    bool is_authority();              // is the calling task the authority?
    bool has_authority();             // is any task the authority?
    int  authority_id();              // authority task id, or -1
    void authority_gone(int task_id); // clear authority if it died

    // Screen geometry (a WM needs the canvas size for its overlay).
    int screen_width();
    int screen_height();

    // Event 'type' values the kernel posts to the authority's mailbox
    // (mirrored in libc grv/syscall.h).
    constexpr uint64_t EVT_SURFACE_ADDED   = 1;
    constexpr uint64_t EVT_SURFACE_REMOVED = 2;

    // Debug overlay: draw the console layer ABOVE the surfaces, so kernel
    // text (faults, F12 dumps, the monitor) stays visible over windows.
    // Changing it repaints immediately.
    void set_console_on_top(bool on);
    void toggle_console_on_top();
    bool console_on_top();

    // Task teardown: release every registry entry owned by 'task_id' and
    // damage the areas they covered. The frames and mappings are freed by
    // the address-space teardown itself.
    void surfaces_for_task_gone(int task_id);

    // Counters for measurement (printed by the F12 dump, read by Sysinfo).
    struct Stats {
        uint64_t presents;    // present() flushes that did work
        uint64_t full_paints; // full-screen paints
        uint64_t rects;       // rectangles painted
        uint64_t pixels;      // pixels written by those rectangles
    };
    Stats stats();
    int surface_count();
    void dump_stats();
}

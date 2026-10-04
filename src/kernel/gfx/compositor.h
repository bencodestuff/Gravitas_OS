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

    // Recomposite: console framebuffer as the background layer, then every
    // registered surface bottom-to-top, then the mouse cursor.
    void composite();

    // Task teardown: release every registry entry owned by 'task_id'. The
    // frames and mappings are freed by the address-space teardown itself.
    void surfaces_for_task_gone(int task_id);

    // Recomposite (skips if nothing changed unless force)
    void composite_if_dirty(bool force);
}

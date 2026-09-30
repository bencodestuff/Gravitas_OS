#pragma once
#include <stdint.h>

// --- Basic compositor ---
// Surfaces are kernel-tracked physical buffers, mapped into the owning
// task's address space. Composite reads them through the HHDM map, so
// it works regardless of which task's CR3 is active — that's what makes
// cross-process z-order and task-exit cleanup possible.

namespace Compositor {

    // Create a w*h 32bpp surface in the calling task's address space.
    // Returns the surface id, or -1. Pixels are at the returned VA
    // (written via the grv_surface_info struct).
    int surface_create(uint64_t w, uint64_t h);

    // Position a surface (top-left corner). Clipped at composite time.
    void surface_move(int id, int x, int y);

    // Recomposite: console framebuffer as the background layer, then every
    // registered surface in creation (z-) order, then the mouse cursor.
    void composite();

    // Task teardown: drop every surface owned by 'task_id'
    void surfaces_for_task_gone(int task_id);

    // Recomposite (skips if nothing changed unless force)
    void composite_if_dirty(bool force);

    // A surface's userland-visible info (address is the OWNER-space VA)
    struct Info {
        uint64_t address;  // valid in the owning task's address space
        uint64_t width;
        uint64_t height;
    };
    bool surface_info(int id, Info* out);
}
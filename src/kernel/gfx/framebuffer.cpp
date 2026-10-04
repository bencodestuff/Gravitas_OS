#include <limine.h>

#include "kernel/gfx/framebuffer.h"
#include "kernel/core/vmm.h"

// Defined in main.cpp — global (NOT in this namespace!)
extern volatile struct limine_hhdm_request hhdm_request;

__attribute__((used, section(".requests")))
volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0
};

namespace Framebuffer {

    // --- Userland graphics support (syscall 16) ---
    // Framebuffer geometry + direct mapping into a target address space.

    bool get_info_and_map(uint64_t cr3, Info* info) {
        if (framebuffer_request.response == nullptr ||
            framebuffer_request.response->framebuffer_count < 1) return false;
        struct limine_framebuffer* fb = framebuffer_request.response->framebuffers[0];

        info->width  = fb->width;
        info->height = fb->height;
        info->pitch  = fb->pitch;
        info->bpp    = fb->bpp;
        info->size   = (uint64_t)fb->height * fb->pitch;

        // The Limine framebuffer address is an HHDM virtual address, so the
        // physical base is simply address - HHDM offset.
        uint64_t phys = (uint64_t)fb->address - hhdm_request.response->offset;
        info->address = 0; // set below

        // Map every 4KB page of the framebuffer into the target space at a
        // fresh contiguous user VA.
        uint64_t pages = (info->size + 4095) / 4096;
        uint64_t base = VMM::alloc_user_range(cr3, pages);
        if (base == 0) return false;

        for (uint64_t p = 0; p < pages; p++) {
            VMM::map_page_in(cr3, phys + p * 4096, base + p * 4096,
                             0x07); // P|W|U
        }
        info->address = base;
        return true;
    }
}

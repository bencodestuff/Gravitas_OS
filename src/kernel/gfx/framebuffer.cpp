#include <limine.h>

#include "kernel/gfx/framebuffer.h"
#include "kernel/core/sched.h"
#include "kernel/core/vmm.h"

// Defined in main.cpp, global (NOT in this namespace!)
extern volatile struct limine_hhdm_request hhdm_request;

__attribute__((used, section(".requests")))
volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0,
    .response = nullptr,
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

        // One user mapping per task, reused for later calls. The address
        // space frees it when the task dies, but without this a task polling
        // FBINFO would reserve a new VA range every time and exhaust the
        // user region. The cache belongs to the space it was made in, so it
        // is only consulted for the current task's cr3.
        bool current_space = (cr3 == Sched::current_task_cr3());
        uint64_t pages = (info->size + 4095) / 4096;
        uint64_t base = 0;
        if (current_space && Sched::current_task_fb_map_pages() == pages)
            base = Sched::current_task_fb_map_base();

        if (base != 0) {
            info->address = base;
            return true;
        }

        base = VMM::alloc_user_range(cr3, pages);
        if (base == 0) return false;

        // PTE_NO_CACHE matches how the kernel maps the same frames into the
        // HHDM (see VMM::init): one physical page must not be cached under
        // two different attributes, and the CPU must not serve stale pixels
        // from a cacheable alias of the scanout.
        const uint64_t flags = VMM::PTE_PRESENT | VMM::PTE_WRITABLE |
                               VMM::PTE_USER | VMM::PTE_NO_CACHE;
        for (uint64_t p = 0; p < pages; p++) {
            VMM::map_page_in(cr3, phys + p * 4096, base + p * 4096, flags);
        }

        if (current_space) Sched::current_task_set_fb_map(base, pages);
        info->address = base;
        return true;
    }
}

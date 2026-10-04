#pragma once
#include <stdint.h>

// --- Framebuffer: hardware access and userland mapping only ---
// Text lives in Console, painting lives in Compositor. This module owns
// just the Limine framebuffer request and the syscall-16 mapping of the
// screen into a user address space.

namespace Framebuffer {
    // --- Info for userland graphics (syscall 16) ---
    struct Info {
        uint64_t width;    // pixels
        uint64_t height;   // pixels
        uint64_t pitch;    // bytes per row
        uint64_t bpp;      // bits per pixel
        uint64_t address;  // USERLAND virtual address (mapped by get_info_and_map)
        uint64_t size;     // total bytes (height * pitch)
    };

    // Fill 'info' with geometry, and map the framebuffer into the address
    // space 'cr3' at a fresh user VA (from the user VA allocator).
    // Returns true on success (info.address then points at the pixels).
    bool get_info_and_map(uint64_t cr3, Info* info);
}

// Global printer helper (klib)
extern "C" void kprint(const char* str);

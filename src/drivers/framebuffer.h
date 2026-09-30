#pragma once
#include <stdint.h>

namespace Framebuffer {
    void init();
    void put_pixel(int x, int y, uint32_t color);
    void print(const char* str);
    void clear_screen(uint32_t color);

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

    // Reset the console cursor to the top-left (a graphics app taking over
    // the screen shouldn't have the text console scribbling mid-screen)
    void reset_cursor();
}

// Global printer helper
extern "C" void kprint(const char* str);
#include <limine.h>

#include "kernel/gfx/framebuffer.h"
#include "kernel/gfx/font.h"
#include "kernel/io.h"
#include "kernel/core/vmm.h"

// Defined in main.cpp — global (NOT in this namespace!)
extern volatile struct limine_hhdm_request hhdm_request;

__attribute__((used, section(".requests")))
volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0
};

namespace Framebuffer {

    // Cursor position
    static int cursor_x = 0;
    static int cursor_y = 0;


    // Scroll the whole console up by one character row (Font::cell_h px).
    // Pixel-level memmove via put_pixel would be brutally slow (each pixel
    // is an MMIO-ish framebuffer write); write rows directly through the
    // framebuffer pointer instead.
    static void scroll_up() {
        if (framebuffer_request.response == nullptr ||
            framebuffer_request.response->framebuffer_count < 1) return;
        struct limine_framebuffer* fb = framebuffer_request.response->framebuffers[0];

        const int cell_h = Font::cell_h();

        uint32_t* base = (uint32_t*)(uintptr_t)fb->address;
        uint32_t pitch_px = fb->pitch / 4;      // pixels per row
        uint32_t width    = fb->width;

        // Move every row below the first up by one cell height
        for (uint32_t y = 0; y + cell_h < fb->height; y++) {
            uint32_t* dst = base + y * pitch_px;
            uint32_t* src = base + (y + cell_h) * pitch_px;
            for (uint32_t x = 0; x < width; x++) dst[x] = src[x];
        }

        // Clear the last cell-height rows
        uint32_t color_bg = 0x00000000;
        for (uint32_t y = fb->height - cell_h; y < fb->height; y++) {
            uint32_t* row = base + y * pitch_px;
            for (uint32_t x = 0; x < width; x++) row[x] = color_bg;
        }
    }

    // Fill the whole framebuffer (row writes, like scroll_up)
    void clear_screen(uint32_t color) {
        if (framebuffer_request.response == nullptr ||
            framebuffer_request.response->framebuffer_count < 1) return;
        struct limine_framebuffer* fb = framebuffer_request.response->framebuffers[0];

        uint32_t* base = (uint32_t*)(uintptr_t)fb->address;
        uint32_t pitch_px = fb->pitch / 4;
        for (uint32_t y = 0; y < fb->height; y++) {
            uint32_t* row = base + y * pitch_px;
            for (uint32_t x = 0; x < fb->width; x++) row[x] = color;
        }
    }

    // Keep the cursor inside the framebuffer; scroll when it runs off the
    // bottom. Returns the (possibly adjusted) cursor_y.
    static int clamp_cursor_y(int cy) {
        if (framebuffer_request.response == nullptr ||
            framebuffer_request.response->framebuffer_count < 1) return cy;
        struct limine_framebuffer* fb = framebuffer_request.response->framebuffers[0];
        while (cy + Font::cell_h() > (int)fb->height) {
            scroll_up();
            cy -= 8;
        }
        return cy;
    }

    void screen_print(const char* str) {
        // 1. Safety check
        if (framebuffer_request.response == nullptr) return;
        struct limine_framebuffer* fb = framebuffer_request.response->framebuffers[0];

        // Colors
        uint32_t color_text = 0x00FFFFFF; // White
        uint32_t color_bg   = 0x00000000; // Black

        for (int i = 0; str[i] != '\0'; i++) {
            char c = str[i];

            if (c == '\b') {
                if (cursor_x >= Font::cell_w()) {
                    cursor_x -= Font::cell_w(); // Move cursor back
                    // Blank the glyph cell (draw with the NUL -> blank path)
                    Font::draw_char(cursor_x, cursor_y, '\0', color_text, color_bg);
                }
                continue; // Move to next character in string
            }

            // --- Handle Newline ---
            if (c == '\n') {
                cursor_x = 0;
                cursor_y = clamp_cursor_y(cursor_y + Font::cell_h());
                continue;
            }

            // --- Draw Printable Characters ---
            if (c >= 32 && c <= 126) {
                cursor_x = Font::draw_char(cursor_x, cursor_y, c, color_text, color_bg);

                // Simple line wrap if we hit the right edge
                if (cursor_x >= (int)fb->width) {
                    cursor_x = 0;
                    cursor_y = clamp_cursor_y(cursor_y + Font::cell_h());
                }
            }
        }
    }


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

        // The Limine framebuffer address is an HHDM virtual address. Find
        // the PHYSICAL base by walking the KERNEL page tables (the HHDM
        // mapping lives there) — frame by frame, since the framebuffer may
        // not be physically contiguous.
        //
        // Simpler: the framebuffer's HHDM address IS physical + HHDM offset.
        // The Limine spec guarantees the framebuffer response's address is
        // the HHDM-mapped virtual address, so:
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

    void reset_cursor() {
        cursor_x = 0;
        cursor_y = 0;
    }
}
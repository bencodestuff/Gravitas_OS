#include <limine.h>

#include "font.h"
#include "framebuffer.h"
#include "kernel/io.h"
#include "kernel/vmm.h"

// Defined in main.cpp — global (NOT in this namespace!)
extern volatile struct limine_hhdm_request hhdm_request;

__attribute__((used, section(".requests")))
volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0
};

namespace Framebuffer {

    static uint64_t hhdm_offset_val() {
        return hhdm_request.response->offset;
    }

    // Forward declare screen_print so `kprint` can call it.
    void screen_print(const char* str);

    extern "C" void kprint(const char* str) {
        // 1. Print to Host Terminal (Serial)
        // We recreate the serial logic here briefly or call a helper
        for (int i = 0; str[i] != '\0'; i++) {
            outb(0x3f8, str[i]); 
        }

        // 2. Print to QEMU Window (Screen)
        screen_print(str);
    }

    // Helper: Put a pixel on the screen
    void put_pixel(int x, int y, uint32_t color) {
        if (framebuffer_request.response == nullptr || framebuffer_request.response->framebuffer_count < 1) return;
        struct limine_framebuffer* fb = framebuffer_request.response->framebuffers[0];

        // Safety check
        if (x < 0 || x >= (int)fb->width || y < 0 || y >= (int)fb->height) return;

        // Calculate memory location: Base + (y * pitch) + (x * bytes_per_pixel)
        uint8_t* base = (uint8_t*) (uintptr_t) fb->address;
        uint64_t offset = (uint64_t)y * fb->pitch + (uint64_t)x * 4;
        uint32_t* ptr = (uint32_t*)(base + offset);
        *ptr = color;
    }

    // Cursor position
    static int cursor_x = 0;
    static int cursor_y = 0;


    // Scroll the whole console up by one character row (8px).
    // Pixel-level memmove via put_pixel would be brutally slow (each pixel
    // is an MMIO-ish framebuffer write); write rows directly through the
    // framebuffer pointer instead.
    static void scroll_up() {
        if (framebuffer_request.response == nullptr ||
            framebuffer_request.response->framebuffer_count < 1) return;
        struct limine_framebuffer* fb = framebuffer_request.response->framebuffers[0];

        uint32_t* base = (uint32_t*)(uintptr_t)fb->address;
        uint32_t pitch_px = fb->pitch / 4;      // pixels per row
        uint32_t width    = fb->width;

        // Move every row below the first up by 8 pixel-rows
        for (uint32_t y = 0; y + 8 < fb->height; y++) {
            uint32_t* dst = base + y * pitch_px;
            uint32_t* src = base + (y + 8) * pitch_px;
            for (uint32_t x = 0; x < width; x++) dst[x] = src[x];
        }

        // Clear the last 8 rows
        uint32_t color_bg = 0x00000000;
        for (uint32_t y = fb->height - 8; y < fb->height; y++) {
            uint32_t* row = base + y * pitch_px;
            for (uint32_t x = 0; x < width; x++) row[x] = color_bg;
        }
    }

    // Keep the cursor inside the framebuffer; scroll when it runs off the
    // bottom. Returns the (possibly adjusted) cursor_y.
    static int clamp_cursor_y(int cy) {
        if (framebuffer_request.response == nullptr ||
            framebuffer_request.response->framebuffer_count < 1) return cy;
        struct limine_framebuffer* fb = framebuffer_request.response->framebuffers[0];
        while (cy + 8 > (int)fb->height) {
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
                if (cursor_x >= 8) {
                    cursor_x -= 8; // Move cursor back
                    // Manually erase the character by drawing a black square
                    for (int y = 0; y < 8; y++) {
                        for (int x = 0; x < 8; x++) {
                            put_pixel(cursor_x + x, cursor_y + y, color_bg);
                        }
                    }
                }
                continue; // Move to next character in string
            }

            // --- Handle Newline ---
            if (c == '\n') {
                cursor_x = 0;
                cursor_y = clamp_cursor_y(cursor_y + 8);
                continue;
            }

            // --- Draw Printable Characters ---
            if (c >= 32 && c <= 126) {
                int index = c - 32; 

                for (int y = 0; y < 8; y++) {
                    uint8_t row = font8x8_basic[index][y];
                    for (int x = 0; x < 8; x++) {
                        // If bit is 1, draw White. If 0, draw Black.
                        if ((row >> (7 - x)) & 1) {
                            put_pixel(cursor_x + x, cursor_y + y, color_text);
                        } else {
                            put_pixel(cursor_x + x, cursor_y + y, color_bg);
                        }
                    }
                }
                cursor_x += 8; 

                // Simple line wrap if we hit the right edge
                if (cursor_x >= (int)fb->width) {
                    cursor_x = 0;
                    cursor_y = clamp_cursor_y(cursor_y + 8);
                }
            }
        }
    }


    // --- Userland graphics support (syscall 16) ---
    // Framebuffer geometry + direct mapping into a target address space.

    static bool read_pte(uint64_t pml4_phys, uint64_t vaddr, uint64_t* out_entry) {
        uint64_t pml4_idx = (vaddr >> 39) & 0x1FF;
        uint64_t pdp_idx  = (vaddr >> 30) & 0x1FF;
        uint64_t pd_idx   = (vaddr >> 21) & 0x1FF;
        uint64_t pt_idx   = (vaddr >> 12) & 0x1FF;

        uint64_t* walk = (uint64_t*)(pml4_phys + hhdm_offset_val());
        uint64_t entry = walk[pml4_idx];
        if (!(entry & 1)) return false;
        walk = (uint64_t*)((entry & 0x000FFFFFFFFFF000) + hhdm_offset_val());
        entry = walk[pdp_idx];
        if (!(entry & 1)) return false;
        walk = (uint64_t*)((entry & 0x000FFFFFFFFFF000) + hhdm_offset_val());
        entry = walk[pd_idx];
        if (!(entry & 1)) return false;
        walk = (uint64_t*)((entry & 0x000FFFFFFFFFF000) + hhdm_offset_val());
        *out_entry = walk[pt_idx];
        return true;
    }

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
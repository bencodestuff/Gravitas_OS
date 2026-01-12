#include <limine.h>

#include "font.h"
#include "framebuffer.h"
#include "kernel/io.h"

__attribute__((used, section(".requests")))
volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0
};

namespace Framebuffer {

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
                cursor_y += 8;
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
                    cursor_y += 8;
                }
            }
        }
    }

}
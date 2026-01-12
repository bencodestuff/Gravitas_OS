#pragma once
#include <stdint.h>

namespace Framebuffer {
    void init();
    void put_pixel(int x, int y, uint32_t color);
    void print(const char* str);
    void clear_screen(uint32_t color);
}

// Global printer helper
extern "C" void kprint(const char* str);
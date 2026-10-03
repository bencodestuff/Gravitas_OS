#pragma once
#include <stdint.h>

// --- 16x16 mouse cursor sprite ---
// Converted from files/Pictures/tile_0026.png (16x16, 2-bit indexed:
// index 0 = transparent via tRNS, 1 = white, 2 = black). Hotspot is the
// arrow tip at (0,0). Encoding matches the original 8x8 compositor sprite:
//   0 = transparent, 1 = black outline, 2 = white fill
// files/Pictures is gitignored, so this header is the tracked artifact.

#define CURSOR_W 16
#define CURSOR_H 16

static const uint8_t cursor_sprite16[CURSOR_H][CURSOR_W] = {
    {0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, 1, 2, 2, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, 1, 2, 2, 2, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, 1, 2, 2, 2, 2, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0},
    {1, 1, 2, 2, 2, 2, 2, 1, 1, 1, 0, 0, 0, 0, 0, 0},
    {1, 1, 2, 2, 2, 2, 2, 2, 1, 1, 1, 0, 0, 0, 0, 0},
    {1, 1, 2, 2, 2, 2, 2, 2, 2, 1, 1, 0, 0, 0, 0, 0},
    {1, 1, 2, 2, 2, 2, 2, 1, 1, 1, 1, 0, 0, 0, 0, 0},
    {1, 1, 2, 2, 1, 2, 2, 2, 1, 1, 0, 0, 0, 0, 0, 0},
    {1, 1, 2, 1, 1, 2, 2, 2, 1, 1, 0, 0, 0, 0, 0, 0},
    {1, 1, 1, 1, 1, 1, 2, 2, 1, 1, 0, 0, 0, 0, 0, 0},
    {0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
};

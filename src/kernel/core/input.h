#pragma once
#include <stdint.h>

// Input events (mirrored in libc grv/input.h).
//
// A task that enables event input receives keyboard (and, through the WM,
// mouse) events as messages in its mailbox; the console keeps the blocking
// GETC character path. See keyboard.cpp for delivery.

namespace Input {
    constexpr uint64_t EV_KEY   = 1; // a = key value, b = modifiers
    constexpr uint64_t EV_MOUSE = 2; // a = buttons, b = (x<<32)|y, window-local

    // Key values: printable ASCII as-is; special keys above ASCII.
    constexpr uint64_t KEY_ESCAPE = 0x100;
    constexpr uint64_t KEY_UP     = 0x101;
    constexpr uint64_t KEY_DOWN   = 0x102;
    constexpr uint64_t KEY_LEFT   = 0x103;
    constexpr uint64_t KEY_RIGHT  = 0x104;

    // Modifier bits (EV_KEY's 'b').
    constexpr uint64_t MOD_SHIFT = 1;
    constexpr uint64_t MOD_CTRL  = 2;
    constexpr uint64_t MOD_ALT   = 4;
    constexpr uint64_t MOD_CAPS  = 8;
}

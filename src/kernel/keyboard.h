#pragma once
#include <stdint.h>

namespace Keyboard {
    // Convert a raw Scancode (Set 1) to an ASCII character
    char translate(uint8_t scancode, bool uppercase);
}
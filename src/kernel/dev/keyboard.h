#pragma once
#include <stdint.h>

namespace Keyboard {
    // Defines
    struct KeyEvent {
        char character;
        bool is_pressed;
    };

    // Called by the hardware interrupt
    void handle_interrupt();

    // Called by the kernel main loop to get the next key
    // Returns 0 if no key is waiting.
    char get_queued_key();
}
#pragma once
#include <stdint.h>

namespace Keyboard {
    // Called by the hardware interrupt
    void handle_interrupt();

    // Called from the timer IRQ: re-deliver a held key (typematic repeat).
    void tick();

    // Called by the kernel main loop to get the next key
    // Returns 0 if no key is waiting.
    char get_queued_key();
}
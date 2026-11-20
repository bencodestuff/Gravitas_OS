#pragma once
#include <stdint.h>

namespace Timer {
    void init();
    void sleep(uint64_t ms); // A simple delay function
    void tick(); // Called by the Interrupt Handler
    uint64_t get_ticks();

    // The PIT runs at 50 Hz, so one tick is 20 ms. Anything that converts a
    // millisecond budget into ticks divides by this (rounding up): a deadline
    // shorter than a tick still has to survive to the next interrupt.
    constexpr uint64_t TICK_MS = 20;
}
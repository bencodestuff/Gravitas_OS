#pragma once
#include <stdint.h>

namespace Timer {
    void init();
    void sleep(uint64_t ms); // A simple delay function
    void tick(); // Called by the Interrupt Handler
    uint64_t get_ticks();
}
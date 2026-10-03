#include "kernel/core/timer.h"
#include "kernel/klib/klib.h"
#include "kernel/io.h"


namespace Timer {
    uint64_t ticks = 0;

    void init() {
        kprint("[PIT] Initializing Timer at 50Hz...\n");

        // 1. Configure the PIT (Port 0x43 = Command, 0x40 = Channel 0)
        // 0x36 = Channel 0 | Access LoByte/HiByte | Mode 3 (Square Wave) | Binary
        outb(0x43, 0x36);

        // 2. Set Frequency to 50 Hz
        // Base clock is 1193180 Hz. Divisor = 1193180 / 50 = 23863
        uint16_t divisor = 1193180 / 50;
        
        // Send Low Byte
        outb(0x40, (uint8_t)(divisor & 0xFF));
        // Send High Byte
        outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
    }

    uint64_t get_ticks() {
        return ticks;
    }
    
    // This will be called by the Interrupt Handler
    void tick() {
        ticks++;
    }

    void sleep(uint64_t ms) {
        // 50 Hz = 20ms per tick.
        uint64_t ticks_to_wait = ms / 20;
        uint64_t target = ticks + ticks_to_wait;
        while (ticks < target) {
            asm("hlt"); // Safe to halt here because the Timer IRQ will wake us up!
        }
    }
}
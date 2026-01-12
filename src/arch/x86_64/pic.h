#pragma once
#include <stdint.h>

namespace PIC {
    // Remap the PIC so IRQs start at offset 32 (0x20)
    void remap();

    // Tell the PIC we are done handling an interrupt (End of Interrupt)
    void send_eoi(uint8_t irq);
    
    // Unmask (enable) a specific IRQ (e.g., 1 for Keyboard)
    void unmask(uint8_t irq);
}
#pragma once
#include <stdint.h>

namespace PIC {
    // Remap the PIC so IRQs start at offset 32 (0x20)
    void remap();

    // Tell the PIC we are done handling an interrupt (End of Interrupt)
    void send_eoi(uint8_t irq);
    
    // Unmask (enable) a specific IRQ (e.g., 1 for Keyboard)
    void unmask(uint8_t irq);
    uint8_t get_mask(uint8_t irq);

    // Silence every IRQ on both PICs. Called before interrupts are enabled,
    // because until remap() runs the PIC's vector mapping is still the
    // firmware's: IRQ0-7 would be delivered as vectors 8-15, so a timer tick
    // would arrive as an exception. The kernel unmasks what it wants.
    void mask_all();
}
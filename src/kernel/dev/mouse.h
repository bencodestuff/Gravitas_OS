#pragma once
#include <stdint.h>

// PS/2 mouse driver (auxiliary device on the 8042 controller).
// Streams 3-byte relative packets: [flags, dx, dy].

namespace Mouse {
    // Enable the aux device, handshake, hook IRQ12. Call once at boot.
    void init();

    // Called from the IRQ12 handler in idt.cpp
    void handle_interrupt();

    // Drain one accumulated packet. Returns true if a packet was ready,
    // filling dx/dy (signed deltas), buttons (bit0 left, bit1 right,
    // bit2 middle) and dz (wheel, signed).
    bool read_packet(int16_t* dx, int16_t* dy, uint8_t* buttons, int8_t* dz);

    // Accumulated cursor position (deltas applied as packets arrive,
    // clamped at composite time to the framebuffer bounds)
    int cursor_x();
    int cursor_y();
    void apply_position_clamp(int x, int y);
}
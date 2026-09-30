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
    // filling dx/dy (signed deltas) and buttons (bit0 left, bit1 right,
    // bit2 middle).
    bool read_packet(int16_t* dx, int16_t* dy, uint8_t* buttons);

    // Non-blocking packet availability peek (for polling loops)
    bool has_packet();
}
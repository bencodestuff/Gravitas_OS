#pragma once
#include <stdint.h>

// Write 8 bits to a port
static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

// Read 8 bits from a port
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// Write 16 bits to a port (ACPI's PM1a control register is 16-bit)
static inline void outw(uint16_t port, uint16_t val) {
    asm volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

// Wait a tiny bit (used for slow hardware like the PIC)
static inline void io_wait(void) {
    outb(0x80, 0);
}
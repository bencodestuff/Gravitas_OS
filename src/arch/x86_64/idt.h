#pragma once
#include <stdint.h>

namespace IDT {
    // Initialize the IDT
    void init();

    // The structure of an IDT Entry (Gate Descriptor)
    struct IDTEntry {
        uint16_t offset_low;       // Lower 16 bits of handler address
        uint16_t selector;         // Kernel Code Segment Selector (0x08)
        uint8_t  ist;              // Interrupt Stack Table (we'll leave 0 for now)
        uint8_t  attributes;       // Type and Attributes (Present, DPL, Gate Type)
        uint16_t offset_mid;       // Middle 16 bits of handler address
        uint32_t offset_high;      // Upper 32 bits of handler address
        uint32_t reserved;         // Must be zero
    } __attribute__((packed));

    // The IDT Register (IDTR) - pointer to the table
    struct IDTR {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed));
}
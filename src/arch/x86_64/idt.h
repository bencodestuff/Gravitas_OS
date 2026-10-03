#pragma once
#include <stdint.h>

namespace IDT {
    // Initialize the IDT
    void init();

    // The interrupted-CPU frame, laid out exactly as the assembly stubs
    // push it: GP registers, then (for exceptions) the vector + error
    // code, then the CPU-pushed frame. 'cs' distinguishes ring 0 from
    // ring 3 — the fault contract keys off it.
    struct Registers {
        // Pushed manually in common stub
        uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
        uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;

        // Pushed by the specific ISR macro
        uint64_t interrupt_number;
        uint64_t error_code;

        // Pushed by the CPU automatically
        uint64_t rip;
        uint64_t cs;
        uint64_t rflags;
        uint64_t rsp;
        uint64_t ss;
    } __attribute__((packed));

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
#include "idt.h"
#include "pic.h"
#include "../../kernel/io.h"
#include "../../kernel/keyboard.h" // <--- NEW

extern "C" void serial_print(const char* str);
extern "C" void serial_print_uint(uint64_t value); 

// Link to the assembly table
extern "C" void* isr_stub_table[];

namespace IDT {

    // This struct matches exactly what is pushed in Assembly
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

    __attribute__((aligned(0x10))) 
    static IDTEntry idt[256];
    static IDTR idtr;

    void set_gate(int interrupt_num, uint64_t handler_addr, uint8_t ist, uint8_t type_attr) {
        idt[interrupt_num].offset_low  = handler_addr & 0xFFFF;
        idt[interrupt_num].selector    = 0x08; 
        idt[interrupt_num].ist         = ist;
        idt[interrupt_num].attributes  = type_attr;
        idt[interrupt_num].offset_mid  = (handler_addr >> 16) & 0xFFFF;
        idt[interrupt_num].offset_high = (handler_addr >> 32) & 0xFFFFFFFF;
        idt[interrupt_num].reserved    = 0;
    }

    void flush_keyboard() {
        // Read status register (0x64)
        // If bit 0 (Output Buffer Full) is set, read the data port (0x60) to burn it.
        while (inb(0x64) & 1) {
            inb(0x60);
        }
    }

    void init() {
        serial_print("[IDT] Initializing...\n");

        idtr.limit = sizeof(idt) - 1;
        idtr.base  = (uint64_t)&idt;

        // Fill the first 48 entries (CPU Exceptions + IRQs)
        // 0x8E = Present(1) | Ring0(00) | Gate(1110 = Interrupt Gate)
        for (int i = 0; i < 48; i++) {
            set_gate(i, (uint64_t)isr_stub_table[i], 0, 0x8E);
        }

        serial_print("[IDT] Loading IDT...\n");
        asm volatile ("lidt %0" : : "m"(idtr)); // Load it!
        serial_print("[IDT] Loaded.\n");
        
        // FLUSH KEYBOARD BEFORE ENABLING INTERRUPTS
        flush_keyboard();

        // Enable interrupts!
        asm volatile ("sti"); 
        serial_print("[IDT] Interrupts Enabled.\n");
    }
}

static uint64_t ticks = 0;

// --- THE C++ HANDLER ---
// This is called by Assembly
extern "C" void interrupt_dispatcher(IDT::Registers* regs) {
    uint64_t num = regs->interrupt_number;
    // --- IRQ 0: Timer (Interrupt 32) ---
    if (num == 32) {
        ticks++;
        // Print a dot roughly every second (Timer runs at ~18Hz)
        if (ticks % 18 == 0) {
            serial_print("Tick...\n"); 
        }
        PIC::send_eoi(0);
        return;
    }
    
    // --- IRQ 1: Keyboard (Interrupt 33) ---
    if (num == 33) {
        uint8_t scancode = inb(0x60);
        
        // Check if a key was RELEASED (Top bit set)
        if (scancode & 0x80) {
            // Key release (e.g., lifting your finger off 'A')
            // We ignore this for now
        } else {
            // Key PRESS
            char c = Keyboard::translate(scancode, false); // false = lowercase for now
            
            if (c != 0) {
                // We have a valid letter!
                char str[2] = {c, '\0'};
                serial_print(str);
            }
        }
        
        PIC::send_eoi(1);
        return;
    }
    serial_print("\n!!! INTERRUPT RECEIVED !!!\n");
    serial_print("Number: "); 
    // Print the numeric interrupt/exception vector
    serial_print_uint(regs->interrupt_number);
    serial_print("\n");

    // Human-friendly names for common exceptions
    if (regs->interrupt_number == 14) {
        serial_print("PAGE FAULT (Bad Memory Access)\n");
    } else if (regs->interrupt_number == 13) {
        serial_print("GPF (General Protection Fault)\n");
    }

    serial_print("Halting CPU.\n");
    while(1) asm("hlt");
}
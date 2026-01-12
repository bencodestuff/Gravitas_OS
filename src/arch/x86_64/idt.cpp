#include "idt.h"
#include "pic.h"
#include "../../kernel/io.h"
#include "../../kernel/keyboard.h"

extern "C" void kprint(const char* str);
extern "C" void kprint_uint(uint64_t value); 

static char input_buffer[256];
static int buffer_index = 0;

// Link to the assembly table
extern "C" void* isr_stub_table[];

bool streq(const char* str1, const char* str2) {
    int i = 0;
    while (str1[i] != '\0' && str2[i] != '\0') {
        if (str1[i] != str2[i]) return false;
        i++;
    }
    return str1[i] == str2[i]; // Check if both ended at the same time
}

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
        kprint("[IDT] Initializing...\n");

        idtr.limit = sizeof(idt) - 1;
        idtr.base  = (uint64_t)&idt;

        // Fill the first 48 entries (CPU Exceptions + IRQs)
        // 0x8E = Present(1) | Ring0(00) | Gate(1110 = Interrupt Gate)
        for (int i = 0; i < 48; i++) {
            set_gate(i, (uint64_t)isr_stub_table[i], 0, 0x8E);
        }

        kprint("[IDT] Loading IDT...\n");
        asm volatile ("lidt %0" : : "m"(idtr)); // Load it!
        kprint("[IDT] Loaded.\n");
        
        // FLUSH KEYBOARD BEFORE ENABLING INTERRUPTS
        flush_keyboard();

        // Enable interrupts!
        asm volatile ("sti"); 
        kprint("[IDT] Interrupts Enabled.\n");
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
            kprint("Tick...\n"); 
        }
        PIC::send_eoi(0);
        return;
    }

 // --- IRQ 1: Keyboard ---
    if (num == 33) {
        uint8_t scancode = inb(0x60);

        if (!(scancode & 0x80)) { // If key PRESS
            char c = Keyboard::translate(scancode, false);

            if (c != 0) {
                // Handle Enter (Newline)
                if (c == '\n') {
                    kprint("\n"); // Move to next line on screen
                    
                    // Terminate the string so we can read it
                    input_buffer[buffer_index] = '\0'; 

                    // --- EXECUTE COMMAND ---
                    if (streq(input_buffer, "hello")) {
                        kprint("Gravitas: Hi there, user!\n");
                    } 
                    else if (streq(input_buffer, "help")) {
                        kprint("Gravitas: Commands: hello, help, clear\n");
                    }
                    else if (streq(input_buffer, "clear")) {
                        // We can't actually clear the serial terminal easily, 
                        // so we just print a bunch of newlines for now.
                        for(int i=0; i<20; i++) kprint("\n");
                    }
                    else if (buffer_index > 0) {
                        kprint("Gravitas: Unknown command.\n");
                    }

                    // Reset buffer for next command
                    buffer_index = 0;
                    kprint("> "); // Print a prompt
                }
                // Handle Backspace
                else if (c == '\b') {
                    if (buffer_index > 0) {
                        buffer_index--;
                        // Hacky way to "delete" on serial: backspace, space, backspace
                        kprint("\b \b"); 
                    }
                }
                // Handle Normal Character
                else {
                    if (buffer_index < 255) {
                        input_buffer[buffer_index++] = c;
                        char str[2] = {c, '\0'};
                        kprint(str); // Echo to screen
                    }
                }
            }
        }
        PIC::send_eoi(1);
        return;
    }

    kprint("\n!!! INTERRUPT RECEIVED !!!\n");
    kprint("Number: "); 
    // Print the numeric interrupt/exception vector
    kprint_uint(regs->interrupt_number);
    kprint("\n");

    // Human-friendly names for common exceptions
    if (regs->interrupt_number == 14) {
        kprint("PAGE FAULT (Bad Memory Access)\n");
    } else if (regs->interrupt_number == 13) {
        kprint("GPF (General Protection Fault)\n");
    }

    kprint("Halting CPU.\n");
    while(1) asm("hlt");
}
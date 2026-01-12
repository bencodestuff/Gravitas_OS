#include "idt.h"
#include "pic.h"
#include "../../kernel/io.h"
#include "../../kernel/keyboard.h"

#include "../../kernel/timer.h"
#include "../../kernel/sched.h"

extern "C" void kprint(const char* str);
extern "C" void kprint_uint(uint64_t value); 

static char input_buffer[256];
static int buffer_index = 0;

// Link to the assembly table
extern "C" void* isr_stub_table[];
// Explicit syscall stub (vector 128)
extern "C" void isr_128();

static inline bool streq(const char* str1, const char* str2) {
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
                // Use IST=1 for the timer IRQ (vector 32) so we always get a
                // known-good kernel stack even when interrupted from user mode.
                uint8_t ist = 0;
                if (i == 32) ist = 1; // Timer
                set_gate(i, (uint64_t)isr_stub_table[i], ist, 0x8E);
        }

        set_gate(0x80, (uint64_t)isr_128, 1, 0xEE);

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

    if (num == 0x80) {
        uint64_t syscall_id = regs->rax;
        uint64_t arg1 = regs->rdi;

        if (syscall_id == 0) { // WRITE (Log)
             kprint("[User] ");
             char c = (char)arg1;
             char str[2] = {c, 0};
             kprint(str);
             kprint("\n");
        }
        else if (syscall_id == 1) { // READ (Keyboard)
             // 1. Try to get a key from the buffer
             char c = Keyboard::get_queued_key();
             
             // 2. Put the result in RAX
             // This is how return values are passed back to User Mode
             regs->rax = (uint64_t)c;
        }
        
        return;
    }

    if (num == 14) {
        uint64_t cr2;
        asm volatile("mov %%cr2, %0" : "=r"(cr2));

        kprint("\n!!! PAGE FAULT !!!\n");
        
        kprint("Faulting Address (CR2): 0x");
        // Simple hex printer
        for(int i=60; i>=0; i-=4) {
             uint64_t n = (cr2 >> i) & 0xF;
             char c = (n < 10) ? '0'+n : 'A'+(n-10);
             char s[2] = {c, '\0'};
             kprint(s);
        }
        kprint("\n");

        kprint("Error Code: ");
        if (regs->error_code & 1) kprint("Present "); else kprint("Not-Present ");
        if (regs->error_code & 2) kprint("Write "); else kprint("Read ");
        if (regs->error_code & 4) kprint("User "); else kprint("Kernel ");
        kprint("\n");
    }

    // --- IRQ 0: Timer (Interrupt 32) ---
    if (num == 32) {
        Timer::tick(); // Update the counter
        PIC::send_eoi(0);

        Sched::yield();

        return;
    }

    // --- IRQ 1: Keyboard ---
    if (num == 33) {
        Keyboard::handle_interrupt(); // Just delegate to the driver
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
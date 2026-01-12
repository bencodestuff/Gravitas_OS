#include <stdint.h>

// Syscall 0: Write Log
void sys_log(char c) {
    asm volatile(
        "mov $0, %%rax\n"
        "mov %0, %%rdi\n"
        "int $0x80\n"
        : : "r"((uint64_t)c) : "rax", "rdi"
    );
}

// Syscall 1: Read Keyboard
// Returns the char, or 0 if buffer is empty
char sys_read() {
    char c;
    asm volatile(
        "mov $1, %%rax\n"  // ID 1
        "int $0x80\n"      // Call Kernel
        "mov %%al, %0\n"   // Read result from AL (low 8 bits of RAX)
        : "=r"(c)          // Output to variable 'c'
        :                  // No inputs
        : "rax"            // Clobbers
    );
    return c;
}

extern "C" void _start() {
    // Print a prompt to show we are ready
    sys_log('>');
    sys_log(' ');

    while(1) {
        // Poll for input
        char c = sys_read();

        // If we got a valid character...
        if (c != 0) {
            // Echo it back to the screen
            sys_log(c);

            // If Enter is pressed, print a new line and a new prompt
            if (c == '\n') {
                sys_log('>');
                sys_log(' ');
            }
        }
        
        // (Optional) Simple "yield" loop so we don't burn 100% CPU
        // In a real OS, sys_read() would block (sleep) until a key is pressed.
        // For now, we just spin.
    }
}
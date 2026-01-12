#include "shell.h"
#include "keyboard.h"
#include "pmm.h" // To print memory stats
#include <stdint.h>

extern "C" void kprint(const char* str);

// Helper: Simple string compare
bool streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == *b;
}

// Helper: Parse Hex String to Int
uint64_t hex2int(const char* str) {
    uint64_t val = 0;
    while (*str) {
        val <<= 4;
        if (*str >= '0' && *str <= '9') val += (*str - '0');
        else if (*str >= 'a' && *str <= 'f') val += (*str - 'a' + 10);
        else if (*str >= 'A' && *str <= 'F') val += (*str - 'A' + 10);
        str++;
    }
    return val;
}

namespace Shell {
    static char cmdbuf[256];
    static int cmdlen = 0;

    void execute() {
        kprint("\n"); // Newline after the user hits Enter

        if (streq(cmdbuf, "help")) {
            kprint("Available commands:\n");
            kprint("  help    - Show this list\n");
            kprint("  clear   - Clear screen\n");
            kprint("  mem     - Show memory stats\n");
            kprint("  crash   - Test IDT (Div by Zero)\n");
            // kprint("  peek <addr> - (Coming soon)\n");
        } 
        else if (streq(cmdbuf, "clear")) {
            // Poor man's clear: scroll a lot
            for(int i=0; i<40; i++) kprint("\n");
        }
        else if (streq(cmdbuf, "mem")) {
            kprint("PMM: Memory is managed. Heap is active.\n");
        }
        else if (streq(cmdbuf, "crash")) {
             volatile int a = 10; volatile int b = 0; volatile int c = a/b;
        }
        else if (cmdbuf[0] == 'p' && cmdbuf[1] == 'e' && cmdbuf[2] == 'e' && cmdbuf[3] == 'k') {
            // Command format: "peek <hex_address>"
            // 1. Find the space
            int space_idx = 4;
            while (cmdbuf[space_idx] == ' ') space_idx++; // Skip spaces
            
            if (cmdbuf[space_idx] == '\0') {
                kprint("Usage: peek <address>\n");
            } else {
                // 2. Parse the address
                uint64_t addr = hex2int(&cmdbuf[space_idx]);
                
                // 3. Print the address header
                kprint("Memory at 0x");
                // (We really need a proper kprint_hex helper soon!)
                // For now, let's assume you trust hex2int worked
                kprint(&cmdbuf[space_idx]); 
                kprint(":\n");

                // 4. Dump 16 bytes
                uint8_t* ptr = (uint8_t*)addr;
                
                // SAFETY: If we peek at bad memory, we crash.
                // In a real OS, we'd check the page tables first!
                // For now, be careful where you peek.
                
                for (int i = 0; i < 16; i++) {
                    uint8_t byte = ptr[i];
                    
                    // Print Hex Byte manually (e.g., "AF ")
                    const char* hex_chars = "0123456789ABCDEF";
                    char byte_str[4];
                    byte_str[0] = hex_chars[(byte >> 4) & 0xF]; // High nibble
                    byte_str[1] = hex_chars[byte & 0xF];        // Low nibble
                    byte_str[2] = ' ';
                    byte_str[3] = '\0';
                    kprint(byte_str);
                }
                kprint("\n");
            }
        }
        else {
            kprint("Unknown command: ");
            kprint(cmdbuf);
            kprint("\n");
        }

        // Reset
        cmdlen = 0;
        kprint("> ");
    }

    void init() {
        kprint("\nGravitas Kernel Shell v0.2\n");
        kprint("> ");
        cmdlen = 0;
    }

    void update() {
        char c = Keyboard::get_queued_key();
        
        if (c != 0) {
            // Handle Backspace
            if (c == '\b') {
                if (cmdlen > 0) {
                    cmdlen--;
                    kprint("\b"); // Visual backspace
                }
            }
            // Handle Enter
            else if (c == '\n') {
                cmdbuf[cmdlen] = '\0';
                execute();
            }
            // Handle Regular Key
            else {
                if (cmdlen < 255) {
                    cmdbuf[cmdlen++] = c;
                    char tmp[2] = {c, '\0'};
                    kprint(tmp); // Echo to screen
                }
            }
        }
    }
}
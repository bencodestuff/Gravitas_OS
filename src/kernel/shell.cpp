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
#include "keyboard.h"
#include "io.h"

namespace Keyboard {

    // US QWERTY Layout (Scancode Set 1)
    // 0 = No key, 0x1E = 'a', etc.
    static const char scan_set_1[] = {
        0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', 
        '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 
        0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, 
        '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '
    };

    // --- CIRCULAR BUFFER ---
    #define BUFFER_SIZE 256
    static char key_buffer[BUFFER_SIZE];
    static volatile int write_ptr = 0;
    static volatile int read_ptr = 0;

    char translate(uint8_t scancode, bool uppercase) {
        if (scancode > 58) return 0;
        return scan_set_1[scancode];
    }

    void handle_interrupt() {
        uint8_t scancode = inb(0x60);

        // Ignore Key Releases (Top bit set) for now
        if (scancode & 0x80) return;

        char c = translate(scancode, false);
        if (c == 0) return;

        // Push to buffer
        int next_write = (write_ptr + 1) % BUFFER_SIZE;
        if (next_write != read_ptr) { // If buffer not full
            key_buffer[write_ptr] = c;
            write_ptr = next_write;
        }
    }

    char get_queued_key() {
        if (read_ptr == write_ptr) return 0; // Buffer empty

        char c = key_buffer[read_ptr];
        read_ptr = (read_ptr + 1) % BUFFER_SIZE;
        return c;
    }
}
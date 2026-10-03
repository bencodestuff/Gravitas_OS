#include "kernel/dev/keyboard.h"
#include "kernel/klib/klib.h"
#include "kernel/io.h"
#include "kernel/core/sched.h"   // F12 debug dump


constexpr uint8_t SC_DEBUG_DUMP = 0x58; // F12 (make code, set 1)

namespace Keyboard {

    // US QWERTY Layout (Scancode Set 1)
    // 0 = No key, 0x1E = 'a', etc.
    static const char scan_set_1[] = {
        0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
        '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
        0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,
        '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '
    };

    // Shifted symbols for the same scancodes (the number row becomes
    // !@#$%^&*(), brackets become {}, etc). Letters are handled by case
    // flip, not this table.
    static const char scan_set_1_shift[] = {
        0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
        '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
        0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,
        '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '
    };

    // --- modifier state ---
    static bool shift_left = false, shift_right = false;
    static bool caps_lock = false;
    static bool extended = false; // 0xE0 prefix (arrows etc) — ignored for now

    // --- CIRCULAR BUFFER ---
    #define BUFFER_SIZE 256
    static char key_buffer[BUFFER_SIZE];
    static volatile int write_ptr = 0;
    static volatile int read_ptr = 0;

    // Scancode make/break constants
    constexpr uint8_t SC_LSHIFT  = 0x2A;
    constexpr uint8_t SC_RSHIFT  = 0x36;
    constexpr uint8_t SC_CAPS    = 0x3A;

    static bool is_letter(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    }

    char translate(uint8_t scancode, bool uppercase) {
        if (scancode > 58) return 0;
        char c = uppercase ? scan_set_1_shift[scancode] : scan_set_1[scancode];
        // Caps lock inverts letter case only (standard behavior):
        // shift+a = A, but caps+a = A too; shift+caps+a = a
        if (is_letter(c) && uppercase) {
            c = (c >= 'a' && c <= 'z') ? (c - 'a' + 'A') : (c - 'A' + 'a');
        }
        return c;
    }

    void handle_interrupt() {
        uint8_t scancode = inb(0x60);

        // Magic debug key (F12): dump the scheduler state and stop. Not
        // enqueued — it's a kernel console command, not a keystroke.
        if (scancode == SC_DEBUG_DUMP) {
            Sched::debug_dump();
            return;
        }

        // 0xE0 prefix: extended keys (arrows, right ctrl/alt) — skip the
        // followup byte entirely for now
        if (extended) { extended = false; return; }
        if (scancode == 0xE0) { extended = true; return; }

        // --- Releases: only matter for shift tracking ---
        if (scancode & 0x80) {
            uint8_t released = scancode & 0x7F;
            if (released == SC_LSHIFT) shift_left = false;
            if (released == SC_RSHIFT) shift_right = false;
            return;
        }

        // --- Modifier makes ---
        if (scancode == SC_LSHIFT) { shift_left = true; return; }
        if (scancode == SC_RSHIFT) { shift_right = true; return; }
        if (scancode == SC_CAPS)   { caps_lock = !caps_lock; return; }

        bool shift = shift_left || shift_right;
        char c = shift ? scan_set_1_shift[scancode] : scan_set_1[scancode];
        if (c == 0) return;
        if (c < 32 && c != '\n' && c != '\b' && c != '\t') {
            return; // don't inject control garbage into the line
        }

        // Caps lock flips LETTER case only: uppercase iff shift XOR caps
        if (is_letter(c) && (shift != caps_lock)) {
            c = (c >= 'a' && c <= 'z') ? (c - 'a' + 'A') : (c - 'A' + 'a');
        }

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
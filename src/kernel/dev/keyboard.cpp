#include "kernel/dev/keyboard.h"
#include "kernel/klib/klib.h"
#include "kernel/io.h"
#include "kernel/core/sched.h"     // focus + event-mode delivery
#include "kernel/core/ipc.h"       // post key events to a task's mailbox
#include "kernel/core/input.h"     // event types, key values, modifiers
#include "kernel/gfx/compositor.h" // F12 compositor stats
#include "kernel/core/timer.h"     // tick deadline for typematic repeat

constexpr uint8_t SC_DEBUG_DUMP = 0x58;     // F12 (make code, set 1)
constexpr uint8_t SC_CONSOLE_ON_TOP = 0x57; // F11 (make code, set 1)

namespace Keyboard {

    // US QWERTY Layout (Scancode Set 1)
    static const char scan_set_1[] = {
        0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
        '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
        0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,
        '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '
    };

    static const char scan_set_1_shift[] = {
        0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
        '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
        0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,
        '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '
    };

    // Scancode make codes tracked as modifiers (set 1).
    constexpr uint8_t SC_LSHIFT = 0x2A, SC_RSHIFT = 0x36, SC_CAPS = 0x3A;
    constexpr uint8_t SC_CTRL   = 0x1D, SC_ALT    = 0x38; // left; right via 0xE0

    static bool shift_left = false, shift_right = false;
    static bool ctrl_left = false, ctrl_right = false;
    static bool alt_left = false, alt_right = false;
    static bool caps_lock = false;
    static bool extended = false; // 0xE0 prefix pending

    // Typematic repeat: the last key-down is re-delivered on a timer while it
    // is held (after a delay, then at a fixed rate). Stored decoded so the
    // repeat re-runs the exact same delivery the make did.
    constexpr uint64_t REPEAT_DELAY = 25; // ticks before the first repeat (500ms)
    constexpr uint64_t REPEAT_RATE  = 2;  // ticks between repeats (40ms, ~25/s)
    static bool     repeat_on = false;
    static uint8_t  repeat_code = 0;
    static bool     repeat_ext = false;
    static uint64_t repeat_value = 0;
    static uint64_t repeat_mods = 0;
    static char     repeat_char = 0;
    static uint64_t repeat_due = 0;

    // Console character ring (the GETC path, unchanged interface).
    #define BUFFER_SIZE 256
    static char key_buffer[BUFFER_SIZE];
    static volatile int write_ptr = 0;
    static volatile int read_ptr = 0;

    static bool is_letter(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    }

    static void push_ring(char c) {
        int next_write = (write_ptr + 1) % BUFFER_SIZE;
        if (next_write != read_ptr) { // not full
            key_buffer[write_ptr] = c;
            write_ptr = next_write;
        }
    }

    // Deliver a decoded key: to the focused event-mode task as a message, or
    // (for the console) to the character ring that GETC drains.
    static void deliver(uint64_t value, uint64_t mods, char ring_char) {
        // A reserved modifier chord goes to the display authority (global
        // hotkeys — the WM's Alt+Tab), whatever holds focus.
        uint64_t grab = (uint64_t)Compositor::hotkey_mask();
        int auth = Compositor::authority_id();
        if (auth > 0 && grab != 0 && (mods & grab) == grab) {
            Ipc::post_kernel(auth, Input::EV_KEY, value, mods);
            return;
        }
        int target = Sched::input_focus();

        // Ctrl+C is the interrupt key, not text: it kills the focused task's
        // foreground job and is consumed. An event-input task (a terminal) still
        // receives it as an event so it can forward it to its hosted console,
        // where CONSOLE_WRITE applies the same rule.
        if (value == 3 && !(target != -1 && Sched::task_event_input(target))) {
            Sched::interrupt_foreground(target);
            return;
        }

        if (target != -1 && Sched::task_event_input(target)) {
            Ipc::post_kernel(target, Input::EV_KEY, value, mods);
            return;
        }
        if (ring_char != 0) {
            push_ring(ring_char);
            Sched::wake_keyboard_waiter();
        }
    }

    void handle_interrupt() {
        uint8_t scancode = inb(0x60);

        // Kernel console commands (make codes only; releases fall through).
        if (scancode == SC_DEBUG_DUMP) {
            Sched::debug_dump();
            Compositor::dump_stats();
            return;
        }
        if (scancode == SC_CONSOLE_ON_TOP) {
            Compositor::toggle_console_on_top();
            return;
        }

        // 0xE0 prefix: the next byte is an extended key (arrows, right
        // ctrl/alt).
        if (scancode == 0xE0) { extended = true; return; }
        bool ext = extended;
        extended = false;

        bool release = (scancode & 0x80) != 0;
        uint8_t code = scancode & 0x7F;

        // --- Modifiers (track press AND release) ---
        if (!ext) {
            if (code == SC_LSHIFT) { shift_left = !release; return; }
            if (code == SC_RSHIFT) { shift_right = !release; return; }
            if (code == SC_CAPS)   { if (!release) caps_lock = !caps_lock; return; }
            if (code == SC_CTRL)   { ctrl_left = !release; return; }
            if (code == SC_ALT)    { alt_left = !release; return; }
        } else {
            if (code == SC_CTRL) { ctrl_right = !release; return; }
            if (code == SC_ALT)  { alt_right = !release; return; }
        }

        // Releasing the held key ends its repeat.
        if (release) {
            if (repeat_on && code == repeat_code && ext == repeat_ext)
                repeat_on = false;
            return;
        }

        uint64_t value = 0;
        if (ext) {
            switch (code) {
                case 0x48: value = Input::KEY_UP; break;
                case 0x50: value = Input::KEY_DOWN; break;
                case 0x4B: value = Input::KEY_LEFT; break;
                case 0x4D: value = Input::KEY_RIGHT; break;
                case 0x49: value = Input::KEY_PAGEUP; break;
                case 0x51: value = Input::KEY_PAGEDOWN; break;
                case 0x47: value = Input::KEY_HOME; break;
                case 0x4F: value = Input::KEY_END; break;
                case 0x53: value = Input::KEY_DELETE; break;
                default: return;
            }
        } else {
            if (code > 58) return;
            bool shift = shift_left || shift_right;
            char c = shift ? scan_set_1_shift[code] : scan_set_1[code];
            if (c == 0) return;
            // Caps lock flips LETTER case only: uppercase iff shift XOR caps.
            if (is_letter(c) && (shift != caps_lock)) {
                c = (c >= 'a' && c <= 'z') ? (c - 'a' + 'A') : (c - 'A' + 'a');
            }
            if (c == 27) value = Input::KEY_ESCAPE;
            else if (c == '\b' || c == '\t' || c == '\n') value = (uint8_t)c;
            else if (c >= 32 && c <= 126) value = (uint8_t)c;
            else return;
        }

        bool shift = shift_left || shift_right;
        bool ctrl = ctrl_left || ctrl_right;
        bool alt = alt_left || alt_right;
        uint64_t mods = (shift ? Input::MOD_SHIFT : 0)
                      | (ctrl ? Input::MOD_CTRL : 0)
                      | (alt ? Input::MOD_ALT : 0)
                      | (caps_lock ? Input::MOD_CAPS : 0);

        // Ctrl+letter -> the control code (a terminal wants ^C, ^D, ...).
        if (ctrl && value >= 'a' && value <= 'z') value = value - 'a' + 1;
        else if (ctrl && value >= 'A' && value <= 'Z') value = value - 'A' + 1;

        // What, if anything, the console ring should see.
        char ring_char = 0;
        if (value >= 32 && value <= 126) ring_char = (char)value;
        else if (value == '\b' || value == '\t' || value == '\n') ring_char = (char)value;

        // Arm typematic repeat: if the key stays down, tick() re-delivers it.
        repeat_on = true;
        repeat_code = code;
        repeat_ext = ext;
        repeat_value = value;
        repeat_mods = mods;
        repeat_char = ring_char;
        repeat_due = Timer::get_ticks() + REPEAT_DELAY;

        deliver(value, mods, ring_char);
    }

    // Called from the timer IRQ: re-deliver the held key once its repeat
    // deadline passes.
    void tick() {
        if (!repeat_on) return;
        uint64_t now = Timer::get_ticks();
        if (now < repeat_due) return;
        repeat_due = now + REPEAT_RATE;
        deliver(repeat_value, repeat_mods, repeat_char);
    }

    char get_queued_key() {
        if (read_ptr == write_ptr) return 0; // empty
        char c = key_buffer[read_ptr];
        read_ptr = (read_ptr + 1) % BUFFER_SIZE;
        return c;
    }
}

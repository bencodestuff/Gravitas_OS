#include "keyboard.h"

namespace Keyboard {

    // US QWERTY Layout (Scancode Set 1)
    // 0 = No key, 0x1E = 'a', etc.
    static const char scan_set_1[] = {
        0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', 
        '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 
        0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, 
        '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '
    };

    char translate(uint8_t scancode, bool uppercase) {
        // If the scancode is too big (outside our array), ignore it
        if (scancode > 58) return 0;

        char c = scan_set_1[scancode];

        // Basic uppercase logic (subtract 32 from lowercase ASCII)
        if (uppercase && c >= 'a' && c <= 'z') {
            c -= 32;
        }

        return c;
    }
}
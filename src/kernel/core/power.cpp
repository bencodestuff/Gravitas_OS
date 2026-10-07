#include "kernel/core/power.h"
#include "kernel/io.h"
#include "kernel/klib/klib.h"

// See power.h. Neither of these returns; if the hardware ignores us we halt
// rather than carry on into a half-shut-down system.

namespace Power {

    static void halt_forever() {
        for (;;) asm volatile("cli; hlt");
    }

    // The machine acts on these asynchronously, so the "ignored" messages below
    // only mean something after a wait long enough to have seen it happen.
    static void settle() {
        for (volatile uint64_t i = 0; i < 200000000; i++) asm volatile("pause");
    }

    void poweroff() {
        asm volatile("cli");
        // ACPI PM1a_CNT: SLP_EN (bit 13). QEMU takes this as S5 and exits.
        // The real port and sleep type come from the FADT.
        outw(0x604, 0x2000);
        // Older QEMU (and Bochs) put the same register at 0xB004.
        outw(0xB004, 0x2000);
        settle();
        kprint("[power] the machine ignored the shutdown\n");
        halt_forever();
    }

    void reboot() {
        asm volatile("cli");
        // The 8042's pulse-reset line: the classic way to reset a PC.
        for (int i = 0; i < 100; i++) {
            if ((inb(0x64) & 0x02) == 0) break; // wait for it to be ready
        }
        outb(0x64, 0xFE);
        settle();
        kprint("[power] the machine ignored the reset\n");
        halt_forever();
    }

} // namespace Power

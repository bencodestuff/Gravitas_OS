#pragma once

// power — leaving. Both of these are one-way: they do not return.
//
// On QEMU the ACPI PM1a control port is 0x604 and the 8042's pulse-reset line
// resets the machine. On real hardware the first would come from the FADT (and
// the second is what a PC does when ACPI is not available), which is a Phase 6
// concern along with flushing anything that needs it first.

namespace Power {
    void poweroff();
    void reboot();
}

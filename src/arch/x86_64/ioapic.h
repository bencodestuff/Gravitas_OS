#pragma once

// The 82093AA I/O APIC: memory-mapped, and a delivery path this kernel does not
// use. It exists here for one reason — the firmware may have left it programmed
// with redirection entries of its own, and an IOAPIC entry carries its own
// vector, so a device interrupt arriving that way is delivered as whatever
// exception number the firmware chose. The PIC is remapped and masked; the
// IOAPIC has to be silenced too, or it bypasses the PIC entirely.

namespace Ioapic {

// Mask every redirection entry. Returns false when there is no IOAPIC at the
// usual address (nothing to do). Logs what it found and what it did.
bool mask_all();

} // namespace Ioapic

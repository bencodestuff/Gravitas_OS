#include "ioapic.h"
#include "kernel/core/vmm.h"

extern "C" void kprint(const char* str);
extern "C" void kprint_uint(uint64_t value);

// --- the I/O APIC ---
//
// Two 32-bit registers form a window: IOREGSEL takes an index, IOWIN reads or
// writes the register it names. Reached through the HHDM (the kernel's direct
// map of physical memory), because the address is fixed by the architecture and
// there is nothing to enumerate.
//
// This is not a driver. The kernel drives interrupts through the PIC, so the
// only thing wanted from the IOAPIC is silence: firmware that left a redirection
// entry unmasked has a live interrupt path with a vector of its own choosing,
// and a vector below 32 is an *exception* number as far as the IDT is concerned.

namespace Ioapic {

constexpr uint64_t MMIO_BASE = 0xFEC00000;
constexpr uint32_t REG_VER   = 0x01;
constexpr uint32_t REDTBL    = 0x10;
constexpr uint32_t MASK_BIT  = 1u << 16;

static volatile uint32_t* reg(uint64_t offset) {
    return (volatile uint32_t*)VMM::phys_to_kernel(MMIO_BASE + offset);
}

// The direct map does not cover MMIO (it is not RAM), so the window has to be
// mapped before it can be touched. Uncached, because a device register is not
// memory.
static bool map_window() {
    uint64_t va = (uint64_t)VMM::phys_to_kernel(MMIO_BASE);
    if (!VMM::is_mapped(va)) {
        VMM::map_page(MMIO_BASE, va,
                      VMM::PTE_PRESENT | VMM::PTE_WRITABLE | VMM::PTE_NO_CACHE);
    }
    return VMM::is_mapped(va);
}

static uint32_t read_reg(uint32_t index) {
    *reg(0x00) = index;
    return *reg(0x10);
}

static void write_reg(uint32_t index, uint32_t value) {
    *reg(0x00) = index;
    *reg(0x10) = value;
}

bool mask_all() {
    if (!map_window()) return false;

    uint32_t ver = read_reg(REG_VER);
    if (ver == 0 || ver == 0xFFFFFFFF) return false;   // nothing there
    uint32_t entries = ((ver >> 16) & 0xFF) + 1;
    if (entries > 64) return false;                    // nonsense: leave it be

    uint32_t live = 0;
    for (uint32_t i = 0; i < entries; i++) {
        uint32_t lo = read_reg(REDTBL + i * 2);
        if (!(lo & MASK_BIT)) live++;
        write_reg(REDTBL + i * 2, lo | MASK_BIT);
    }

    kprint("[IOAPIC] ");
    kprint_uint(entries);
    kprint(" entries, ");
    kprint_uint(live);
    kprint(" unmasked by the firmware, now all masked\n");
    return true;
}

} // namespace Ioapic

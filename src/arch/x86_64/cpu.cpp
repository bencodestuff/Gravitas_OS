#include "cpu.h"
#include <stdint.h>

namespace Cpu {
    void enable_fpu_sse() {
        uint64_t cr0, cr4;

        // CR0: EM off (no x87 emulation), TS off (no lazy FPU switching),
        // MP on (WAIT/FWAIT faults only with TS), NE on (native #MF above
        // the timer IRQ vector instead of legacy IRQ13 reporting).
        asm volatile("mov %%cr0, %0" : "=r"(cr0));
        cr0 &= ~((uint64_t)1 << 2); // EM
        cr0 &= ~((uint64_t)1 << 3); // TS
        cr0 |=  ((uint64_t)1 << 1); // MP
        cr0 |=  ((uint64_t)1 << 5); // NE
        asm volatile("mov %0, %%cr0" : : "r"(cr0));

        // CR4: OSFXSR makes fxsave/fxrstor cover the XMM registers (the
        // per-task FPU area is 512 bytes, which is exactly the FXSAVE
        // layout). OSXMMEXCPT turns unmasked SSE faults into #XF.
        asm volatile("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= ((uint64_t)1 << 9);  // OSFXSR
        cr4 |= ((uint64_t)1 << 10); // OSXMMEXCPT
        asm volatile("mov %0, %%cr4" : : "r"(cr4));
    }

    // The local APIC: this kernel drives interrupts through the 8259 PIC, so a
    // local APIC that the firmware left enabled is a delivery path the kernel
    // does not own. That matters because the firmware also leaves the IOAPIC
    // programmed, and an IOAPIC redirection entry carries its own vector: a
    // device interrupt arriving that way is delivered as whatever vector the
    // firmware chose, with no error code, so the IDT sees a *different*
    // exception than the one that fired. (Seen here: a timer tick delivered as
    // vector 8, a bogus "double fault" whose frame was one slot out because the
    // #DF stub expects an error code the CPU never pushed.) Clearing
    // IA32_APIC_BASE.EN closes the path; the PIC is the only source left.
    void disable_local_apic() {
        constexpr uint32_t IA32_APIC_BASE = 0x1B;
        uint32_t lo, hi;
        asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(IA32_APIC_BASE));
        lo &= ~(1u << 11);              // APIC global enable
        asm volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(IA32_APIC_BASE));
    }
}
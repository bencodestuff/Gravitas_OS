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
}
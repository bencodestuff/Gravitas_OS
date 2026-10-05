#pragma once

// CPU-level setup that must happen before anything else uses the FPU/SSE.

namespace Cpu {
    // Enable the x87 FPU and SSE: clear CR0.EM/TS, set CR0.MP/NE, and set
    // CR4.OSFXSR (so fxsave/fxrstor include the XMM state) + OSXMMEXCPT.
    // Call once at boot, before the scheduler snapshots the FPU template.
    void enable_fpu_sse();
}
#pragma once
#include <stdint.h>
#include "arch/x86_64/idt.h"

// --- Fault reporter + the "userland cannot hang the machine" contract ---
//
// One module owns everything about CPU exceptions:
//   * the diagnostic dump (vector name, registers, task, CR2)
//   * the CONTRACT: a ring-3 fault terminates the offending task through
//     the normal scheduler exit path and the system carries on; a ring-0
//     fault is a kernel bug — dump and halt, honestly and visibly.
//   * the stack-protector trap (kernel side)
//
// All output goes through kprint, so it reaches serial even when the
// visible framebuffer is owned by userland graphics.

namespace Fault {

    // Seed the stack-protector guard with boot-time entropy. Call once,
    // at the very top of kmain, before any protected function runs.
    void init();

    // Handle a CPU exception (vectors 0..31). Never returns:
    //   ring 3 -> the task is killed (Sched::exit_current, EXIT_FAULT)
    //   ring 0 -> full dump, then the CPU halts with interrupts off
    // 'regs' is the interrupted frame; cr2 is meaningful for #PF only.
    [[noreturn]] void handle_cpu_exception(IDT::Registers* regs, uint64_t cr2);

    // Kernel-side __stack_chk_fail: report the offending caller and halt.
    [[noreturn]] void stack_chk_fail_kernel(uint64_t caller_rip);
}

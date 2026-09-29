#pragma once
#include <stdint.h>

namespace Sched {
    void sleep(uint64_t ms);
    void init();
    void create_task(void(*entry_point)());
    void yield(); // Voluntarily give up CPU

    // --- Process lifecycle ---
    // Build a ring-3 task: 'rip' is the userland entry point (from the ELF
    // loader), 'cr3' is its pre-cloned address space (ELF code must already
    // be mapped into it — do that with CR3 pointed at the new space).
    // 'argc'/'argv' (kernel-side strings) are copied onto the child's
    // initial user stack in System V layout, so _start(argc, argv)
    // receives its arguments. Returns task id.
    int create_user_task(uint64_t rip, const char* name, int argc,
                         const char* const* argv, uint64_t cr3);

    // Sleep the current task until the keyboard IRQ pushes a key.
    // (Caller must have interrupts disabled around the check-and-block.)
    void block_on_keyboard();

    // Wake a task blocked on the keyboard (called from the keyboard IRQ).
    void wake_keyboard_waiter();

    // Put the current task to sleep until task 'child_id' has exited.
    void block_current(int child_id);

    // Tear down the current task, wake whoever was waiting on it, and
    // switch to the next ready task. Never returns.
    void exit_current();
}
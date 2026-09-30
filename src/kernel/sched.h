#pragma once
#include <stdint.h>

#include "../fs/vfs.h" // fd tables are per-task (Unix style), vfs.h defines MAX_FDS

namespace Sched {
    void sleep(uint64_t ms);
    void init();
    void create_task(void(*entry_point)());
    void yield(); // Voluntarily give up CPU

    // Returns the current task's fd table (array of MAX_FDS pointers)
    VFS::FileHandle** current_task_fds();

    const char* current_task_cwd();  // read the current task's CWD
    void current_task_set_cwd(const char* path); // store a canonical path

    // --- Process lifecycle ---
    // Build a ring-3 task: 'rip' is the userland entry point (from the ELF
    // loader), 'cr3' is its pre-cloned address space (ELF code must already
    // be mapped into it — do that with CR3 pointed at the new space).
    // 'argc'/'argv' (kernel-side strings) are copied onto the child's
    // initial user stack in System V layout, so _start(argc, argv)
    // receives its arguments. Returns task id.
    int create_user_task(uint64_t rip, const char* name, int argc,
                         const char* const* argv, uint64_t cr3);

    // Program break accessors for the syscall layer (Task is private here)
    uint64_t current_task_brk();
    void current_task_set_brk(uint64_t value);

    // Sleep the current task until the keyboard IRQ pushes a key.
    // (Caller must have interrupts disabled around the check-and-block.)
    void block_on_keyboard();

    // Wake a task blocked on the keyboard (called from the keyboard IRQ).
    void wake_keyboard_waiter();

    // Put the current task to sleep until task 'child_id' has exited.
    void block_current(int child_id);

    // Sleep the current task for 'ticks' timer ticks (timer IRQ wakes it).
    void sleep_current(uint64_t ticks);

    // Tear down the current task, wake whoever was waiting on it, and
    // switch to the next ready task. Never returns.
    void exit_current();
}
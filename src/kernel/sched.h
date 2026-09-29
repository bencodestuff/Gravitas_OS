#pragma once
#include <stdint.h>

namespace Sched {
    void sleep(uint64_t ms);
    void init();
    void create_task(void(*entry_point)());
    void yield(); // Voluntarily give up CPU

    // --- Process lifecycle ---
    // Build a ring-3 task: 'rip' is the userland entry point (from the ELF
    // loader). The user stack is allocated and mapped here. Returns task id.
    int create_user_task(uint64_t rip, const char* name);

    // Put the current task to sleep until task 'child_id' has exited.
    void block_current(int child_id);

    // Tear down the current task, wake whoever was waiting on it, and
    // switch to the next ready task. Never returns.
    void exit_current();
}
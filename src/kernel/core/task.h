#pragma once
#include <stdint.h>

#include "fs/vfs.h"

// task.h — the task object + scheduler-private global state.
// Internal to Sched: sched.cpp (queue + context switch) and proc.cpp
// (lifecycle) both include it; NOBODY ELSE does — outside modules use
// the Sched public API in sched.h, or their own module's state.

namespace Sched {

    enum TaskState {
        READY,
        SLEEPING
    };

    struct Task {
        uint64_t rsp;
        Task* next;
        int id;
        TaskState state;
        uint64_t wake_timestamp;
        char name[16];

        // --- MISSING FIELD ---
        uint64_t kstack_top; // We need this for the TSS!
        uint64_t* kstack_mem; // Base of the heap-allocated kernel stack (for freeing)

        // --- FPU/SSE context ---
        // Eager save/restore in switch_context. The area needs 16-byte
        // alignment for fxsave, but the kernel heap only guarantees 8 —
        // so it's a separate aligned allocation (fxsave_raw is the
        // unaligned base kept for delete[]).
        uint8_t* fxsave_area;    // 512 bytes, 16+ aligned
        uint8_t* fxsave_raw;     // allocation base

        // --- Process lifecycle ---
        int waiting_on;         // task id this task is blocked on (-1 = none)
        uint64_t user_rip;      // for the userland trampoline
        uint64_t user_rsp;
        uint64_t cr3;           // this task's address space
        uint64_t brk;           // program break (malloc arena top), 0 = unset
        uint64_t brk_limit;     // end of the MAPPED brk region (page-aligned)
        VFS::FileHandle* fds[VFS::MAX_FDS]; // per-task file descriptors
        char cwd[64];           // current working directory (starts "/")
        uint64_t exit_code;     // value passed to exit(), or a fault code
                                // if the task died from an exception
        uint64_t exit_code_inbox;   // exit code of the task this task WAITED
                                    // on (written by exit_current at kill time)
    };

    // Shared scheduler state. Storage lives in sched.cpp; proc.cpp
    // reaches everything below through these declarations.
    extern Task* current_task;
    extern Task* ready_queue;    // circular ready list (Round Robin)
    extern Task* zombie;         // exited task awaiting cleanup by the next switch
    extern int task_counter;     // id issuer (storage in sched.cpp)
    extern uint8_t fpu_template[512]; // boot-clean FPU snapshot (storage in sched.cpp)

    // Switch the address space to 'task' (no-op if already current).
    // Used by yield (sched.cpp) and exit_current (proc.cpp).
    void switch_to_task_cr3(Task* task);
}

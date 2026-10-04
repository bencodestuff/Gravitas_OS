#pragma once
#include <stdint.h>

#include "fs/vfs.h" // fd tables are per-task (Unix style), vfs.h defines MAX_FDS

namespace Sched {
    void sleep(uint64_t ms);
    void init();
    void create_task(void(*entry_point)());
    void yield(); // Voluntarily give up CPU

    // Returns the current task's fd table (array of MAX_FDS pointers)
    VFS::FileHandle** current_task_fds();

    const char* current_task_cwd();  // read the current task's CWD
    void current_task_set_cwd(const char* path); // store a canonical path
    int current_task_id();
    const char* current_task_name();

    // Kernel debugger: print the task table to the console (serial + fb).
    // Read-only walk; safe from IRQ context.
    void debug_dump();

    // --- Introspection (debugger/monitor; read-only snapshots) ---
    struct TaskInfo {
        int id;
        int state;          // 0 ready, 1 sleeping, 2 running (this snapshot)
        uint64_t cr3;
        uint64_t brk;
        int64_t waiting_on; // task being waited on, or -1
        char name[16];
    };
    // Fill up to 'max' records; returns the number written.
    int task_snapshot(TaskInfo* out, int max);
    int task_count();

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
    uint64_t current_task_brk_limit();
    void current_task_set_brk_limit(uint64_t value);

    // Sleep the current task until the keyboard IRQ pushes a key.
    // (Caller must have interrupts disabled around the check-and-block.)
    void block_on_keyboard();

    // Wake a task blocked on the keyboard (called from the keyboard IRQ).
    void wake_keyboard_waiter();

    // Put the current task to sleep until task 'child_id' has exited.
    void block_current(int child_id);

    // Sleep until something explicitly calls wake_task(our id) — used by
    // IPC recv (and later wait kinds). Unlike block_current() this sets no
    // waiting-on-task relation, so no exit can wake us as a side effect;
    // ONLY wake_task does. Caller must cli-guard the check-and-block
    // window (same discipline as block_on_keyboard).
    void block_until_woken();

    // Wake a task that is blocked in an explicit-wait (block_until_woken).
    // Returns true if it was found and woken. Does nothing (and returns
    // false) for tasks sleeping on the timer, on the keyboard, or that do
    // not exist — their wakeup belongs to their own mechanism.
    bool wake_task(int id);

    // Sleep the current task for 'ticks' timer ticks (timer IRQ wakes it).
    void sleep_current(uint64_t ticks);

    // Current task's address space (syscall layer needs it for fb mapping)
    uint64_t current_task_cr3();

    // Tear down the current task, wake whoever was waiting on it, and
    // switch to the next ready task. Never returns. 'code' is stored as
    // the task's exit code (readable by the parent once waitpid exists;
    // faults pass a distinctive GRV_EXIT_* value).
    void exit_current(uint64_t code);

    // Exit code of the task the current task WAITED on (or 0)
    uint64_t current_exit_inbox();

    // Consume a recorded recent exit (WAIT for an already-exited task).
    // Returns false if no exit records this id. The record is consumed —
    // each id can be waited on once.
    bool consume_recent_exit(int id, uint64_t* out_code);

    // Terminate ANOTHER task by id (v1: any task may terminate any other).
    // Refuses the kernel task and self-kill. 'code' becomes the victim's
    // exit code. Returns 0 or -1.
    int kill_task(int id, uint64_t code);

    // Does a task with this id exist? (WAIT uses it to decide between
    // blocking on a live task and failing on an unknown one.)
    bool task_exists(int id);

    // Exit codes with meaning beyond "the program returned N". ASCII
    // spells (FAULT / SMASH) so they read themselves in a hex dump.
    constexpr uint64_t EXIT_FAULT = 0x4641554C54ull;      // "FAULT"
    constexpr uint64_t EXIT_STACK_SMASH = 0x534D415348ull; // "SMASH"
}
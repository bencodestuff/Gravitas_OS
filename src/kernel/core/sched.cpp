// sched.cpp — the RUN QUEUE and context switching: init (kernel task +
// boot FPU snapshot), yield (wake sweep, zombie reap, switch), the
// blocking/waking primitives, keyboard-wait plumbing, and the debug dump.
// Process LIFECYCLE (create/exit/kill/WAIT) lives in proc.cpp; the two
// share the Task definition and shared state via core/task.h.

#include "task.h"

#include "kernel/core/sched.h"
#include "kernel/klib/klib.h"
#include "kernel/core/heap.h"
#include "kernel/core/timer.h"
#include "kernel/core/vmm.h"
#include "arch/x86_64/gdt.h"
#include "kernel/gfx/compositor.h"
#include "fs/vfs.h"
#include "kernel/core/ipc.h"

extern "C" void switch_context(uint64_t* old_sp, uint64_t next_sp,
                               void* old_fxsave, void* new_fxsave);

namespace Sched {

    int task_counter = 0; // id issuer (see task.h)

    // Boot-clean FPU snapshot, seeded by init(); every task's fxsave area
    // is seeded from it so the first fxrstor is always valid.
    uint8_t fpu_template[512] __attribute__((aligned(64)));

    // Shared scheduler state (see task.h for the declarations).
    Task* current_task = nullptr;
    Task* ready_queue = nullptr;
    Task* zombie = nullptr;   // exited task awaiting cleanup by the next switch

    void init() {
        // ... previous setup ...
        current_task = new Task;
        current_task->id = 0;
        current_task->state = READY;
        strcpy(current_task->name, "Kernel Shell");

        // --- FPU: snapshot the boot-clean state as the template for all
        // future tasks. Limine leaves the FPU enabled + initialized; a
        // task whose fxsave_area was never saved would otherwise fxrstor
        // heap garbage (-> #GP on first float use).
        for (int i = 0; i < 512; i++) fpu_template[i] = 0;
        asm volatile("fxsave %0" : : "m"(fpu_template) : "memory");

        // The kernel-main task gets an fxsave area too (timer switch saves it)
        current_task->fxsave_raw = new uint8_t[512 + 63];
        current_task->fxsave_area = (uint8_t*)(((uint64_t)current_task->fxsave_raw + 63) & ~(uint64_t)63);
        for (int i = 0; i < 512; i++) current_task->fxsave_area[i] = fpu_template[i];
        
        // --- THE FIX ---
        // Capture the current stack pointer so the TSS knows where to land
        // when we return to this task from User Mode.
        uint64_t current_rsp;
        asm volatile("mov %%rsp, %0" : "=r"(current_rsp));
        current_task->kstack_top = current_rsp;
        current_task->rsp = current_rsp; // valid for self-switches
        current_task->cr3 = VMM::get_kernel_pml4_phys();

        // Initialize ALL per-task state the spawn path normally sets —
        // the kernel task is hand-built here, and uninitialized fields
        // get INHERITED by the first userland process (the shell's CWD
        // used to be boot garbage: prompts printed 0x01 bytes and "cd"
        // broke in ways that took a scancode trace to find)
        current_task->brk = 0;
        current_task->brk_limit = 0;
        current_task->exit_code = 0;
        current_task->waiting_on = -1;
        current_task->user_rip = 0;
        current_task->user_rsp = 0;
        current_task->cwd[0] = '/'; current_task->cwd[1] = '\0';
        for (int i = 0; i < VFS::MAX_FDS; i++) current_task->fds[i] = nullptr;
        // ----------------
        
        ready_queue = current_task;
        current_task->next = current_task;
        
        kprint("[Sched] Initialized.\n");
    }


    void block_current(int child_id) {
        current_task->state = SLEEPING;
        current_task->waiting_on = child_id;
        // Never wake this task by timestamp — only the child's exit wakes us
        current_task->wake_timestamp = (uint64_t)-1;
    }

    void block_until_woken() {
        current_task->state = SLEEPING;
        current_task->waiting_on = -1;   // no exit may wake us as a side effect
        current_task->wake_timestamp = (uint64_t)-1; // nor the timer
    }

    bool wake_task(int id) {
        Task* t = ready_queue;
        do {
            if (t->id == id) {
                // Only tasks blocked in block_until_woken respond here:
                // timer-sleepers and keyboard-waiters keep their own wake
                // discipline (and a task blocked on a task exit keeps its
                // waiting_on contract). 'waiting_on == -1' guards that too.
                if (t->state == SLEEPING && t->waiting_on == -1 &&
                    t->wake_timestamp == (uint64_t)-1) {
                    t->state = READY;
                    return true;
                }
                return false;
            }
            t = t->next;
        } while (t != ready_queue);
        return false;
    }

    void sleep_current(uint64_t ticks) {
        // Timestamp-based: yield()'s wake loop flips us back to READY when
        // Timer::get_ticks() passes the target. waiting_on stays -1 (no
        // process should wake us early).
        if (ticks == 0) return;
        current_task->state = SLEEPING;
        current_task->waiting_on = -1;
        current_task->wake_timestamp = Timer::get_ticks() + ticks;
    }

    void debug_dump() {
        kprint("[dbg] tasks (ticks=");
        kprint_uint(Timer::get_ticks());
        kprint("):\n");
        Task* t = ready_queue;
        if (t == nullptr) { kprint("[dbg]   (none)\n"); return; }
        do {
            kprint("[dbg]   #");
            kprint_uint(t->id);
            kprint(" '");
            kprint(t->name);
            kprint("' ");
            if (t->state == SLEEPING) {
                if (t->waiting_on >= 0) {
                    kprint("blocked-on-task-");
                    kprint_uint(t->waiting_on);
                } else {
                    kprint("sleeping");
                }
            } else {
                kprint(t == current_task ? "RUNNING" : "ready");
            }
            kprint(" cr3=");
            kprint_hex(t->cr3);
            kprint("\n");
            t = t->next;
        } while (t != ready_queue);
    }

    int task_count() {
        Task* t = ready_queue;
        if (t == nullptr) return 0;
        int n = 0;
        do { n++; t = t->next; } while (t != ready_queue);
        return n;
    }

    int task_snapshot(TaskInfo* out, int max) {
        if (out == nullptr || max <= 0) return 0;
        Task* t = ready_queue;
        if (t == nullptr) return 0;
        int n = 0;
        do {
            if (n >= max) break;
            out[n].id = t->id;
            out[n].state = (t == current_task) ? 2 : (int)t->state;
            out[n].cr3 = t->cr3;
            out[n].brk = t->brk;
            out[n].waiting_on = t->waiting_on;
            for (int i = 0; i < 16; i++) out[n].name[i] = t->name[i];
            n++;
            t = t->next;
        } while (t != ready_queue);
        return n;
    }

    static Task* keyboard_waiter = nullptr; // task blocked in the getc syscall

    void block_on_keyboard() {
        current_task->state = SLEEPING;
        current_task->wake_timestamp = (uint64_t)-1; // only the IRQ wakes us
        keyboard_waiter = current_task;
    }

    void wake_keyboard_waiter() {
        if (keyboard_waiter != nullptr) {
            keyboard_waiter->state = READY;
            keyboard_waiter = nullptr;
        }
    }

    void switch_to_task_cr3(Task* task) {
        if (task->cr3 != VMM::current_pml4()) {
            VMM::switch_pml4(task->cr3);
        }
    }

    void yield() {

        if (current_task == nullptr) return;

        Task* old_task = current_task;
        Task* next_task = current_task->next;
        
        // --- LOOP UNTIL WE FIND A READY TASK ---
        // If everyone is sleeping, we just spin (idle loop)
        while (true) {
            // 1. Check if a sleeping task is done sleeping
            if (next_task->state == SLEEPING) {
                if (Timer::get_ticks() >= next_task->wake_timestamp) {
                    next_task->state = READY; // Wakey wakey!
                }
            }

            // 2. Is this task ready to run?
            if (next_task->state == READY) {
                break; // Found one!
            }

            // 3. Move to next
            next_task = next_task->next;
        }

        // Only one runnable task (or we looped back to ourselves)?
        // Then there is nothing to switch — just keep running.
        if (old_task == next_task) return;

        // Switch to the found task
        current_task = next_task;
        GDT::tss.rsp0 = current_task->kstack_top;
        switch_to_task_cr3(next_task);
        switch_context(&old_task->rsp, next_task->rsp,
                       old_task->fxsave_area, next_task->fxsave_area);

        // We only get here when this task is scheduled back in. CALLERS may
        // have interrupted-off around the block (spawn/wait do cli before
        // blocking) — if we leaked IF=0 into another task's kernel context,
        // the PIT would count on OUR time budget and sleeping everywhere
        // would freeze. Re-enable always: every resume path returns through
        // a trap/I RET whose saved user flags carry IF=1 anyway.
        asm volatile("sti");
        // Reap any task that exited since the last switch (its old kernel
        // stack is definitely abandoned by now). CLI guards 'zombie': a
        // nested PIT yield could otherwise reap on our behalf and null the
        // pointer mid-loop.
        asm volatile("cli");
        if (zombie != nullptr) {
            // Close any fds the task leaked, then free its address space
            for (int i = 0; i < VFS::MAX_FDS; i++) {
                if (zombie->fds[i] != nullptr) {
                    VFS::close_fd_for(zombie->fds, i);
                }
            }
            Compositor::surfaces_for_task_gone(zombie->id);
            Ipc::task_gone(zombie->id);
            // Free the exited task's address space (its user frames + tables)
            VMM::destroy_user_space(zombie->cr3);
            delete[] zombie->kstack_mem;
            delete[] zombie->fxsave_raw;
            delete zombie;
            zombie = nullptr;
        }
        asm volatile("sti");
    }

} // namespace Sched

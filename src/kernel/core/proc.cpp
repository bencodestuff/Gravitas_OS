// proc.cpp — task LIFECYCLE, split from sched.cpp (which owns the run
// queue and context switching): creation (kernel thread / userland task),
// exit, kill, the WAIT bookkeeping, and the per-task accessors the
// syscall layer uses. Both files share Task through core/task.h.

#include "task.h"

#include "sched.h"
#include "kernel/klib/klib.h"
#include "kernel/core/heap.h"
#include "kernel/core/pmm.h"
#include "kernel/core/vmm.h"
#include "kernel/core/timer.h"
#include "kernel/gfx/compositor.h"
#include "fs/vfs.h"
#include "kernel/core/ipc.h"
#include "arch/x86_64/gdt.h"

// Assembly trampoline: runs a forged user task's first switch_context frame
// (R12 = user RIP, R13 = user RSP) and iretq's into ring 3.
extern "C" void user_entry_trampoline();

extern "C" void switch_context(uint64_t* old_sp, uint64_t next_sp,
                               void* old_fxsave, void* new_fxsave);

// task_counter lives in sched.cpp (init() starts it at zero)
extern int task_counter;

// fpu_template lives in sched.cpp (init() snapshots the boot-clean FPU)
extern uint8_t fpu_template[512];

namespace Sched {

    // Per-task user stack: 8 pages (32 KB). Big enough for frames that hold
    // a few KB of scratch (a WM keeping surface records, a shell's buffers).
    constexpr uint64_t USER_STACK_PAGES = 8;

    // --- WAIT for already-exited tasks ---
    // WAIT is advisory (a parent may wait late or never), so every exit
    // lands in a small ring that WAIT consumes. Bounded: once 8 newer
    // exits push an id out, an unwatched late WAIT returns -1 (Unix
    // waitpid has the same behavior once the child is reaped).
    constexpr int RECENT_EXITS = 8;
    struct RecentExit { int id; uint64_t code; };
    static RecentExit recent_exits[RECENT_EXITS];
    static uint64_t exit_head = 0;    bool task_exists(int id) {
        if (current_task == nullptr) return false;
        Task* t = ready_queue;
        do {
            if (t->id == id) return true;
            t = t->next;
        } while (t != ready_queue);
        return false;
    }

    // WAIT step 1: consume a matching recent exit. Returns false (with
    // out_code untouched) when no entry matches this id — the caller next
    // checks task_exists() and finally fails with -1.
    bool consume_recent_exit(int id, uint64_t* out_code) {
        for (int i = 0; i < RECENT_EXITS; i++) {
            if (recent_exits[i].id == id) {
                if (out_code != nullptr) *out_code = recent_exits[i].code;
                recent_exits[i].id = -1;
                return true;
            }
        }
        return false;
    }

    uint64_t current_exit_inbox() {
        return current_task->exit_code_inbox;
    }


    // only current_task executes), so its kernel stack is abandoned and
    // we can tear everything down synchronously — address space, fds,
    // surfaces, stacks — no zombie hop needed.
    //   code: what its exit_code becomes (callers blocked on it receive it)
    //   Returns 0, or -1 for unknown/kernel-self/self-kill.
    int kill_task(int id, uint64_t code) {
        if (id <= 0) return -1;                       // kernel task / invalid
        if (current_task != nullptr && id == current_task->id)
            return -1;                                // use exit(), not kill()

        asm volatile("cli");  // list surgery must not interleave with the timer's yield
        Task* victim = nullptr;
        Task* prev = ready_queue;
        do {
            if (prev->next != nullptr && prev->next->id == id) { victim = prev->next; break; }
            prev = prev->next;
        } while (prev != ready_queue);

        if (victim == nullptr) { asm volatile("sti"); return -1; }
        victim->exit_code = code;

        char namebuf[16];
        strcpy(namebuf, victim->name);

        // Same bookkeeping as a normal exit: ring + waiter wakeups
        recent_exits[exit_head % RECENT_EXITS].id = victim->id;
        recent_exits[exit_head % RECENT_EXITS].code = victim->exit_code;
        exit_head++;
        Task* t = ready_queue;
        do {
            if (t->waiting_on == victim->id) {
                t->exit_code_inbox = victim->exit_code;
                t->state = READY;
                t->waiting_on = -1;
                consume_recent_exit(victim->id, nullptr);
            }
            t = t->next;
        } while (t != ready_queue);

        // Unlink (the caller itself stays in the queue).
        Task* before = victim;
        while (before->next != victim) before = before->next;
        before->next = victim->next;
        if (ready_queue == victim) ready_queue = victim->next;

        // Synchronous teardown: the victim's stack is not in use.
        for (int i = 0; i < VFS::MAX_FDS; i++) {
            if (victim->fds[i] != nullptr) VFS::close_fd_for(victim->fds, i);
        }
        Compositor::surfaces_for_task_gone(victim->id);
        Compositor::authority_gone(victim->id);
        input_focus_gone(victim->id);
        console_owner_gone(victim->id);
        Ipc::task_gone(victim->id);
        VMM::destroy_user_space(victim->cr3);
        delete[] victim->kstack_mem;
        delete[] victim->fxsave_raw;
        delete victim;

        kprint("[Sched] Task '");
        kprint(namebuf);
        kprint("' terminated (killed).\n");
        asm volatile("sti");
        return 0;
    }

    void create_task(void(*entry_point)()) {
        // 1. Allocate a new Task struct
        Task* new_task = new Task;
        new_task->id = ++task_counter;
        new_task->state = READY;
        new_task->wake_timestamp = 0;
        new_task->getc_waiting = false;
        new_task->event_input = false;
        new_task->console_owner = -1;
        new_task->con_in_head = new_task->con_in_tail = 0;
        new_task->con_out_head = new_task->con_out_tail = 0;
        new_task->con_notified = false;
        new_task->timer_deadline = 0;
        new_task->timer_armed = false;

        // 2. Allocate a Stack for this thread (4KB)
        uint64_t* stack_bottom = new uint64_t[4096]; // 32KB: deep syscalls
                                                     // + nested IRQs live here
        uint64_t* stack_top = stack_bottom + 512;

        new_task->kstack_top = (uint64_t)stack_top;

        // 3. Forge the Stack!
        // We need to match exactly what 'switch_context' expects to pop.
        // Order: RBX, RBP, R12, R13, R14, R15, RIP (Return Address)
        
        stack_top--; *stack_top = (uint64_t)entry_point; // RIP (where to start)
        stack_top--; *stack_top = 0; // R15
        stack_top--; *stack_top = 0; // R14
        stack_top--; *stack_top = 0; // R13
        stack_top--; *stack_top = 0; // R12
        stack_top--; *stack_top = 0; // RBP
        stack_top--; *stack_top = 0; // RBX

        new_task->rsp = (uint64_t)stack_top;
        new_task->kstack_mem = stack_bottom;
        new_task->waiting_on = -1;
        new_task->exit_code = 0;

        // 4. Add to the circular linked list
        new_task->next = ready_queue->next;
        ready_queue->next = new_task;
        
        kprint("[Sched] Task Created.\n");
    }


    int create_user_task(uint64_t rip, const char* name, int argc,
                         const char* const* argv, uint64_t cr3) {
        Task* new_task = new Task;
        new_task->id = ++task_counter;
        new_task->state = READY;
        new_task->wake_timestamp = 0;
        new_task->waiting_on = -1;
        new_task->getc_waiting = false;
        new_task->event_input = false;
        new_task->console_owner = current_task->console_owner; // inherit stdio
        new_task->con_in_head = new_task->con_in_tail = 0;
        new_task->con_out_head = new_task->con_out_tail = 0;
        new_task->con_notified = false;
        new_task->timer_deadline = 0;
        new_task->timer_armed = false;
        new_task->exit_code = 0;
        new_task->cr3 = cr3;
        strcpy(new_task->name, name);

        // Kernel stack (interrupts from ring 3 land here via the TSS). 32KB
        // so a deep syscall (printf -> PUTS -> kprint -> compositor present)
        // plus a nested IRQ cannot overrun it.
        uint64_t* stack_bottom = new uint64_t[4096];
        uint64_t* stack_top = stack_bottom + 4096;
        new_task->kstack_top = (uint64_t)stack_top;
        new_task->kstack_mem = stack_bottom;

        // User stack: allocated from the per-space VA allocator (no more
        // id-based fixed slots — two processes can overlap freely since
        // they own separate address spaces, and one process can't).
        uint64_t old_cr3 = VMM::current_pml4();
        bool switched = false;
        if (old_cr3 != new_task->cr3) {
            VMM::switch_pml4(new_task->cr3);
            switched = true;
        }
        uint64_t stack_base = VMM::alloc_user_range(new_task->cr3, USER_STACK_PAGES);
        if (stack_base == 0) {
            kprint("[Sched] Out of user VA for stack!\n");
            if (switched) VMM::switch_pml4(old_cr3);
            // NOTE: caller must clean up the task on failure (id already used)
            return -1;
        }
        // Back every page of the stack: alloc_user_range only reserves the
        // VA range, so the frames must be mapped here. 8 pages (32K) is a
        // comfortable floor — bigger programs put multi-KB frames on it.
        for (uint64_t p = 0; p < USER_STACK_PAGES; p++)
            VMM::map_page(PMM::alloc_page(), stack_base + p * 4096, 0x07); // P|W|U
        if (switched) VMM::switch_pml4(old_cr3);

        // --- Build the initial user stack (System V argv layout) ---
        // [rsp]     = argc
        // [rsp+8..] = argv[0..argc-1], argv[argc] = NULL
        // higher    = the argv string bytes themselves
        static uint64_t str_addrs[8];
        uint64_t total_len = 0;
        for (int i = 0; i < argc; i++) {
            const char* s = argv[i];
            while (*s) { total_len++; s++; }
            total_len++; // NUL
        }
        uint64_t stack_top_va = stack_base + USER_STACK_PAGES * 4096;
        uint64_t strings_start = (stack_top_va - total_len) & ~(uint64_t)15;
        uint64_t array_start = strings_start - ((argc + 1) * 8);
        uint64_t new_rsp = array_start - 8;

        char* dst = (char*)strings_start;
        for (int i = 0; i < argc; i++) {
            str_addrs[i] = (uint64_t)dst;
            const char* s = argv[i];
            while (*s) *dst++ = *s++;
            *dst++ = '\0';
        }
        for (int i = 0; i < argc; i++) ((uint64_t*)array_start)[i] = str_addrs[i];
        ((uint64_t*)array_start)[argc] = 0; // argv[argc] = NULL
        *(uint64_t*)new_rsp = (uint64_t)argc;

        new_task->user_rip = rip;
        new_task->user_rsp = new_rsp;
        new_task->brk = 0; // 0 = unset; brk syscall initializes on first use
        new_task->brk_limit = 0;
        // CWD is inherited from the spawning process (Unix semantics) —
        // otherwise `cd /bin` + `ls` would list "/" in every child
        strcpy(new_task->cwd, current_task->cwd);

        // FPU area: separately allocated (heap is only 8-aligned; fxsave
        // needs 16) and seeded with the boot-clean template so the first
        // fxrstor is valid
        new_task->fxsave_raw = new uint8_t[512 + 63];
        new_task->fxsave_area = (uint8_t*)(((uint64_t)new_task->fxsave_raw + 63) & ~(uint64_t)63);
        for (int i = 0; i < 512; i++) new_task->fxsave_area[i] = fpu_template[i];
        for (int i = 0; i < VFS::MAX_FDS; i++) new_task->fds[i] = nullptr;

        // Forge the switch_context stack. NOTE: switch_context pops in
        // REVERSE push order (r15 first, rbx last), so the slots must be
        // written deepest-first as: ret, rbx, rbp, r12, r13, r14, r15.
        // R12/R13 carry the userland RIP/RSP into the trampoline; R14/R15
        // carry argc/argv, which the trampoline moves into RDI/RSI.
        stack_top--; *stack_top = (uint64_t)user_entry_trampoline; // deepest: ret target
        stack_top--; *stack_top = 0;            // rbx
        stack_top--; *stack_top = 0;            // rbp
        stack_top--; *stack_top = rip;          // r12 -> user RIP
        stack_top--; *stack_top = new_rsp;      // r13 -> user RSP
        stack_top--; *stack_top = (uint64_t)argc;  // r14 -> argc
        stack_top--; *stack_top = array_start;  // r15 -> argv
        new_task->rsp = (uint64_t)stack_top;

        new_task->next = ready_queue->next;
        ready_queue->next = new_task;

        kprint("[Sched] User task '");
        kprint(name);
        kprint("' created.\n");
        return new_task->id;
    }


    uint64_t current_task_brk() {
        return current_task->brk;
    }

    void current_task_set_brk(uint64_t value) {
        current_task->brk = value;
    }

    uint64_t current_task_brk_limit() {
        return current_task->brk_limit;
    }

    void current_task_set_brk_limit(uint64_t value) {
        current_task->brk_limit = value;
    }

    VFS::FileHandle** current_task_fds() {
        return current_task->fds;
    }

    uint64_t current_task_cr3() {
        return current_task->cr3;
    }

    int current_task_id() {
        return current_task->id;
    }

    const char* current_task_name() {
        return current_task->name;
    }

    // Kernel debugger: dump the task table (F12 / fault diagnostics).

    const char* current_task_cwd() {
        return current_task->cwd;
    }

    // chdir: validate that the target exists and is a directory (done by
    // the caller via VFS), then store the canonical path
    void current_task_set_cwd(const char* path) {
        int i = 0;
        while (path[i] && i < 63) { current_task->cwd[i] = path[i]; i++; }
        current_task->cwd[i] = '\0';
    }

    void exit_current(uint64_t code) {
        Task* dead = current_task;
        dead->exit_code = code;
        kprint("[Sched] Task '");
        kprint(dead->name);
        kprint("' exited.\n");

        // 1. Record this exit for WAIT: append to the recent-exits ring
        //    (waiters that call WAIT after the fact scan this).
        recent_exits[exit_head % RECENT_EXITS].id = dead->id;
        recent_exits[exit_head % RECENT_EXITS].code = dead->exit_code;
        exit_head++;

        // 2. Wake anyone blocked on this task (and give them the code).
        //    A waking waiter consumes the ring entry — the deliverable is
        //    the inbox now, and WAIT on this id must not resolve twice.
        Task* t = ready_queue;
        do {
            if (t->waiting_on == dead->id) {
                t->exit_code_inbox = dead->exit_code;
                t->state = READY;
                t->waiting_on = -1;
                consume_recent_exit(dead->id, nullptr);
            }
            t = t->next;
        } while (t != ready_queue);

        // 3. Last task standing? Nothing to switch to — halt forever.
        if (dead->next == dead) {
            kprint("[Sched] No tasks left. Halting.\n");
            while (1) asm("hlt");
        }

        // 4. Unlink from the circular list
        Task* prev = dead;
        while (prev->next != dead) prev = prev->next;
        prev->next = dead->next;
        if (ready_queue == dead) ready_queue = dead->next;

        // 5. Pick the next ready task (single CPU: no SMP races here)
        Task* next = dead->next;
        while (next->state != READY) {
            if (next->state == SLEEPING &&
                Timer::get_ticks() >= next->wake_timestamp) {
                next->state = READY;
                break;
            }
            next = next->next;
        }

        // 6. Free the Task struct AFTER the next switch (we are standing on
        // the dying kernel stack, so we can't free it here).
        zombie = dead;

        current_task = next;
        GDT::tss.rsp0 = next->kstack_top;
        switch_to_task_cr3(next);
        uint64_t dummy_rsp; // never used — this task ceases to exist
        switch_context(&dummy_rsp, next->rsp, dead->fxsave_area, next->fxsave_area);
        // Never returns: the CPU is now on the next task's stack.
        while (1) asm("hlt");
    }

} // namespace Sched

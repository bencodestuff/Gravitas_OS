#include "sched.h"
#include "heap.h"
#include "timer.h"
#include "arch/x86_64/gdt.h"
#include "pmm.h"
#include "vmm.h"
#include "../fs/vfs.h"

// Assembly trampoline: runs a forged user task's first switch_context frame
// (R12 = user RIP, R13 = user RSP) and iretq's into ring 3.
extern "C" void user_entry_trampoline();

// Minimal freestanding strcpy implementation for the kernel (C linkage)
extern "C" char* strcpy(char* dest, const char* src) {
    char* ret = dest;
    while ((*dest++ = *src++) != '\0') {
        // copy
    }
    return ret;
}

extern "C" void kprint(const char* str);
extern "C" void switch_context(uint64_t* old_sp, uint64_t next_sp);



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

        // --- Process lifecycle ---
        int waiting_on;         // task id this task is blocked on (-1 = none)
        uint64_t user_rip;      // for the userland trampoline
        uint64_t user_rsp;
        uint64_t cr3;           // this task's address space
        uint64_t brk;           // program break (malloc arena top), 0 = unset
        VFS::FileHandle* fds[VFS::MAX_FDS]; // per-task file descriptors
        char cwd[64];           // current working directory (starts "/")
    };

    Task* current_task = nullptr;
    Task* ready_queue = nullptr; // A simple linked list (Round Robin)
    int task_counter = 0;
    static Task* zombie = nullptr; // exited task awaiting cleanup by the next switch

    void sleep(uint64_t ms) {
        // 1. Calculate target time
        // Assuming 50Hz (20ms per tick)
        uint64_t ticks_needed = ms / 20;
        current_task->wake_timestamp = Timer::get_ticks() + ticks_needed;

        // 2. Mark as SLEEPING
        current_task->state = SLEEPING;

        // 3. Give up the CPU immediately
        yield();
    }

    void init() {
        // ... previous setup ...
        current_task = new Task;
        current_task->id = 0;
        current_task->state = READY;
        strcpy(current_task->name, "Kernel Shell");
        
        // --- THE FIX ---
        // Capture the current stack pointer so the TSS knows where to land
        // when we return to this task from User Mode.
        uint64_t current_rsp;
        asm volatile("mov %%rsp, %0" : "=r"(current_rsp));
        current_task->kstack_top = current_rsp; 
        current_task->rsp = current_rsp; // valid for self-switches
        current_task->cr3 = VMM::get_kernel_pml4_phys();
        // ----------------
        
        ready_queue = current_task;
        current_task->next = current_task;
        
        kprint("[Sched] Initialized.\n");
    }

    void create_task(void(*entry_point)()) {
        // 1. Allocate a new Task struct
        Task* new_task = new Task;
        new_task->id = ++task_counter;
        new_task->state = READY;
        new_task->wake_timestamp = 0;

        // 2. Allocate a Stack for this thread (4KB)
        uint64_t* stack_bottom = new uint64_t[512]; // 512 * 8 bytes = 4KB
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
        new_task->cr3 = cr3;
        strcpy(new_task->name, name);

        // Kernel stack (interrupts from ring 3 land here via the TSS)
        uint64_t* stack_bottom = new uint64_t[512];
        uint64_t* stack_top = stack_bottom + 512;
        new_task->kstack_top = (uint64_t)stack_top;
        new_task->kstack_mem = stack_bottom;

        // User stack: unique virtual address per process.
        // 0x500000 is reserved for the legacy hand-launched program; tasks
        // start at 0x600000. Map the page itself plus the one below it, since
        // the stack pointer sits at the top of the page and early pushes go
        // downwards.
        uint64_t user_stack_va = 0x500000 + (uint64_t)new_task->id * 0x100000;
        VMM::map_page(PMM::alloc_page(), user_stack_va, 0x07);        // P|W|U
        VMM::map_page(PMM::alloc_page(), user_stack_va - 0x1000, 0x07);

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
        uint64_t strings_start = (user_stack_va - total_len) & ~(uint64_t)15;
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
        // CWD is inherited from the spawning process (Unix semantics) —
        // otherwise `cd /bin` + `ls` would list "/" in every child
        strcpy(new_task->cwd, current_task->cwd);
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

    void block_current(int child_id) {
        current_task->state = SLEEPING;
        current_task->waiting_on = child_id;
        // Never wake this task by timestamp — only the child's exit wakes us
        current_task->wake_timestamp = (uint64_t)-1;
    }

    uint64_t current_task_brk() {
        return current_task->brk;
    }

    void current_task_set_brk(uint64_t value) {
        current_task->brk = value;
    }

    VFS::FileHandle** current_task_fds() {
        return current_task->fds;
    }

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

    static void switch_to_task_cr3(Task* task) {
        if (task->cr3 != VMM::current_pml4()) {
            VMM::switch_pml4(task->cr3);
        }
    }

    void exit_current() {
        Task* dead = current_task;
        kprint("[Sched] Task '");
        kprint(dead->name);
        kprint("' exited.\n");

        // 1. Wake anyone blocked on this task
        Task* t = ready_queue;
        do {
            if (t->waiting_on == dead->id) {
                t->state = READY;
                t->waiting_on = -1;
            }
            t = t->next;
        } while (t != ready_queue);

        // 2. Last task standing? Nothing to switch to — halt forever.
        if (dead->next == dead) {
            kprint("[Sched] No tasks left. Halting.\n");
            while (1) asm("hlt");
        }

        // 3. Unlink from the circular list
        Task* prev = dead;
        while (prev->next != dead) prev = prev->next;
        prev->next = dead->next;
        if (ready_queue == dead) ready_queue = dead->next;

        // 4. Pick the next ready task (single CPU: no SMP races here)
        Task* next = dead->next;
        while (next->state != READY) {
            if (next->state == SLEEPING &&
                Timer::get_ticks() >= next->wake_timestamp) {
                next->state = READY;
                break;
            }
            next = next->next;
        }

        // 5. Free the Task struct AFTER the next switch (we are standing on
        // the dying kernel stack, so we can't free it here).
        zombie = dead;

        current_task = next;
        GDT::tss.rsp0 = next->kstack_top;
        switch_to_task_cr3(next);
        uint64_t dummy_rsp; // never used — this task ceases to exist
        switch_context(&dummy_rsp, next->rsp);
        // Never returns: the CPU is now on the next task's stack.
        while (1) asm("hlt");
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
        switch_context(&old_task->rsp, next_task->rsp);

        // We only get here when this task is scheduled back in.
        // Reap any task that exited since the last switch (its old kernel
        // stack is definitely abandoned by now).
        if (zombie != nullptr) {
            // Close any fds the task leaked, then free its address space
            for (int i = 0; i < VFS::MAX_FDS; i++) {
                if (zombie->fds[i] != nullptr) {
                    VFS::close_fd_for(zombie->fds, i);
                }
            }
            // Free the exited task's address space (its user frames + tables)
            VMM::destroy_user_space(zombie->cr3);
            delete[] zombie->kstack_mem;
            delete zombie;
            zombie = nullptr;
        }
    }
}
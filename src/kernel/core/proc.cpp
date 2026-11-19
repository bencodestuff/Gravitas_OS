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
#include "kernel/core/env.h"
#include "kernel/core/timer.h"
#include "kernel/gfx/compositor.h"
#include "fs/vfs.h"
#include "kernel/core/ipc.h"
#include "kernel/core/memmap.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "kernel/core/errno.h"

// Assembly trampoline: runs a forged user task's first switch_context frame
// (R12 = user RIP, R13 = user RSP) and iretq's into ring 3.
extern "C" void user_entry_trampoline();
// Restores the switch_context frame of a forked child and the interrupt frame
// behind it (see fork_current).
extern "C" void fork_restore_entry();

extern "C" void switch_context(uint64_t* old_sp, uint64_t next_sp,
                               void* old_fxsave, void* new_fxsave);

// task_counter lives in sched.cpp (init() starts it at zero)
extern int task_counter;

// fpu_template lives in sched.cpp (init() snapshots the boot-clean FPU)
extern uint8_t fpu_template[512];

namespace Sched {

    // Per-task user stack: 8 pages (32 KB). Big enough for frames that hold
    // a few KB of scratch (a WM keeping surface records, a shell's buffers).
    constexpr uint64_t USER_STACK_PAGES = 32;

    // Bounded copy into a fixed-size name field: a program name can be longer
    // than the field (and `new Task` leaves it uninitialized otherwise).
    static void copy_name(char* dst, const char* src, int cap) {
        int i = 0;
        while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
        dst[i] = '\0';
    }

    // --- fork ---

    // Release a half-built forked child. Only used before the task is linked
    // into the ready queue, so nothing else can be looking at it. Pointers are
    // cleared as they are allocated, so every failure path can call this.
    static void discard_forked_child(Task* child, uint64_t cr3) {
        if (child != nullptr) {
            for (int i = 0; i < VFS::MAX_FDS; ++i)
                if (child->fds[i] != nullptr) VFS::close_fd_for(child->fds, i);
            delete[] child->fxsave_raw;
            delete[] child->kstack_mem;
            delete child;
        }
        if (cr3 != 0) VMM::destroy_user_space(cr3);
    }

    // Build a task that returns from the syscall its parent is inside.
    //
    // spawn forges a fresh entry point and user stack; a forked child must
    // come back out of the very trap it was copied from, at the same user
    // instruction and on the same user stack, with rax = 0 to tell it apart
    // from the parent. The child therefore owns a private copy-on-write
    // address space and its own kernel stack, whose first words are the
    // switch_context frame (callee-saved registers and the return address)
    // followed by an exact copy of the parent's interrupt frame.
    // fork_restore_entry pops that frame and iretqs, so the child never
    // re-enters interrupt_dispatcher and the scheduler's post-yield
    // bookkeeping does not run twice for one trap.
    //
    // Returns the new task id in the parent; the child sees 0. The task is
    // linked into the ready queue as the very last step, after every
    // allocation has succeeded, so a failure leaves nothing half-published.
    static_assert(sizeof(IDT::Registers) % sizeof(uint64_t) == 0,
                  "the child interrupt frame is copied in 8-byte words");

    int fork_current(IDT::Registers* parent_frame) {
        if (parent_frame == nullptr || current_task == nullptr ||
            current_task->id == 0) return -Errno::EINVAL;   // ring-3 tasks only

        // A shared region owns its frames and tracks holders by task id. A
        // cloned space would point at them with no mapping record and no
        // reference, so the parent close or exit could release them while the
        // child still reads them. Refuse before anything is built: the caller
        // unmaps and tries again.
        if (MemMap::holds_region_mapping(current_task->id))
            return -Errno::EOPNOTSUPP;

        asm volatile("cli" ::: "memory");
        Task* parent = current_task;

        uint64_t child_cr3 = VMM::clone_user_space(parent->cr3);
        if (child_cr3 == 0) return -Errno::ENOMEM;

        Task* child = new Task;
        if (child == nullptr) {
            VMM::destroy_user_space(child_cr3);
            return -Errno::ENOMEM;
        }
        child->kstack_mem = nullptr;
        child->fxsave_raw = nullptr;
        for (int i = 0; i < VFS::MAX_FDS; ++i) child->fds[i] = nullptr;

        // Everything spawn normally derives from an argv is inherited from the
        // parent instead. Unset fields here would be read by the scheduler.
        child->id = ++task_counter;
        child->state = READY;
        child->wake_timestamp = 0;
        child->next = nullptr;
        copy_name(child->name, parent->name, (int)sizeof(child->name));
        child->waiting_on = -1;
        child->getc_waiting = false;
        child->event_input = parent->event_input;
        child->user_rip = parent_frame->rip;   // same continuation as the parent
        child->user_rsp = parent_frame->rsp;   // same user stack
        child->user_stack_guard = parent->user_stack_guard;
        child->cr3 = child_cr3;
        child->brk = parent->brk;
        child->brk_limit = parent->brk_limit;
        child->fb_map_base = parent->fb_map_base;
        child->fb_map_pages = parent->fb_map_pages;
        child->exit_code = 0;
        child->exit_code_inbox = 0;
        child->parent_id = parent->id;
        child->zombie_next = nullptr;
        child->console_owner = parent->console_owner;
        child->con_in_head = child->con_in_tail = 0;
        child->con_out_head = child->con_out_tail = 0;
        child->con_notified = false;
        child->timer_deadline = 0;
        child->timer_armed = false;
        for (int i = 0; i < (int)sizeof(child->cwd); ++i)
            child->cwd[i] = parent->cwd[i];

        // Its own kernel stack: the child trap frames must not sit on top of
        // the parent ones still in flight.
        child->kstack_mem = new uint64_t[4096];
        if (child->kstack_mem == nullptr) {
            discard_forked_child(child, child_cr3);
            return -Errno::ENOMEM;
        }
        child->kstack_mem[0] = KSTACK_CANARY;
        child->kstack_top = (uint64_t)(child->kstack_mem + 4096);

        // FPU/SSE: snapshot the live state. The saved area of the parent is
        // only written when it is switched out, so it can be stale here; the
        // kernel itself runs with SSE disabled, so the registers still hold
        // the userland state.
        child->fxsave_raw = new uint8_t[512 + 63];
        if (child->fxsave_raw == nullptr) {
            discard_forked_child(child, child_cr3);
            return -Errno::ENOMEM;
        }
        child->fxsave_area = (uint8_t*)(((uint64_t)child->fxsave_raw + 63) &
                                        ~(uint64_t)63);
        struct FxArea { uint8_t bytes[512]; } __attribute__((aligned(64)));
        FxArea live;
        asm volatile("fxsave %0" : "=m"(live) : : "memory");
        for (int i = 0; i < 512; ++i)
            child->fxsave_area[i] = live.bytes[i];

        // Descriptors: shared open file descriptions, one reference each, so a
        // pipe end or a file offset is shared with the parent. Nothing here
        // can fail, so it runs after every allocation.
        VFS::fds_inherit(child->fds);

        // switch_context pops r15 first, so its frame is stored deepest-last:
        // six callee-saved words, then the return address that enters
        // fork_restore_entry, then the interrupt frame it restores.
        uint64_t* frame = child->kstack_mem + 4096 -
                          (sizeof(IDT::Registers) / sizeof(uint64_t));
        IDT::Registers saved = *parent_frame;
        saved.rax = 0;                       // the fork result seen by the child
        const uint64_t* words = (const uint64_t*)&saved;
        for (uint64_t i = 0; i < sizeof(saved) / sizeof(uint64_t); ++i)
            frame[i] = words[i];

        uint64_t* context = frame - 7;
        for (int i = 0; i < 6; ++i) context[i] = 0;   // callee-saved registers
        context[6] = (uint64_t)fork_restore_entry;    // switch_context return
        child->rsp = (uint64_t)context;

        // Publication: the last step, with interrupts still off from the
        // syscall entry, so the timer cannot switch to a half-built task.
        child->next = ready_queue->next;
        ready_queue->next = child;
        return child->id;
    }

    // --- Child status (parent-owned WAIT) ---
    // A child exit is kept until its parent collects it, even when the child
    // is already gone. The table is small and fixed: a new record overwrites
    // the oldest, so a parent that never waits loses the status instead of
    // growing the kernel. Only the parent may collect a record; any other
    // task gets the same -1 as an unknown id.
    constexpr int CHILD_EXITS = 8;
    struct ChildExit { int parent; int id; uint64_t code; };
    static ChildExit child_exits[CHILD_EXITS];
    static int child_exit_next = 0;    // overwrite cursor, oldest first

    static void record_child_exit(int parent, int id, uint64_t code) {
        child_exits[child_exit_next].parent = parent;
        child_exits[child_exit_next].id = id;
        child_exits[child_exit_next].code = code;
        child_exit_next = (child_exit_next + 1) % CHILD_EXITS;
    }

    // Drop an id record without delivering it: the exit already went into the
    // parent inbox, so WAIT on this id must not resolve a second time.
    static void drop_child_exit(int id) {
        for (int i = 0; i < CHILD_EXITS; i++)
            if (child_exits[i].id == id) child_exits[i].id = -1;
    }

    // A dead parent can never collect, so its records are dropped with it.
    static void drop_child_exits_of(int parent) {
        for (int i = 0; i < CHILD_EXITS; i++)
            if (child_exits[i].parent == parent) child_exits[i].id = -1;
    }

    bool task_exists(int id) {
        if (current_task == nullptr) return false;
        Task* t = ready_queue;
        do {
            if (t->id == id) return true;
            t = t->next;
        } while (t != ready_queue);
        return false;
    }

    bool task_is_child(int id) {
        if (current_task == nullptr || id <= 0) return false;
        Task* t = ready_queue;
        do {
            if (t->id == id) return t->parent_id == current_task->id;
            t = t->next;
        } while (t != ready_queue);
        return false;
    }

    // WAIT step 1: collect a recorded exit belonging to the caller. Returns
    // false (out_code untouched) for another task child, an unknown id, or an
    // id already collected.
    bool consume_child_exit(int id, uint64_t* out_code) {
        for (int i = 0; i < CHILD_EXITS; i++) {
            if (child_exits[i].id == id &&
                child_exits[i].parent == current_task->id) {
                if (out_code != nullptr) *out_code = child_exits[i].code;
                child_exits[i].id = -1;
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
    // Save/restore the interrupt flag. kill_task runs both from the KILL
    // syscall (IF off) and from the keyboard IRQ (IF off), so it must never
    // switch interrupts back on in the middle of an interrupt handler.
    static inline uint64_t irq_save() {
        uint64_t f;
        asm volatile("pushfq; popq %0; cli" : "=r"(f) :: "memory");
        return f;
    }
    static inline void irq_restore(uint64_t f) {
        if (f & (1ull << 9)) asm volatile("sti"); else asm volatile("cli");
    }

    int kill_task(int id, uint64_t code) {
        if (id <= 0) return -1;                       // kernel task / invalid
        if (current_task != nullptr && id == current_task->id)
            return -1;                                // use exit(), not kill()

        uint64_t iflags = irq_save(); // list surgery must not interleave with the timer's yield
        Task* victim = nullptr;
        Task* prev = ready_queue;
        do {
            if (prev->next != nullptr && prev->next->id == id) { victim = prev->next; break; }
            prev = prev->next;
        } while (prev != ready_queue);

        if (victim == nullptr) { irq_restore(iflags); return -1; }
        victim->exit_code = code;

        // Sized from the field, not a guess: Task::name is 32 bytes and a
        // spawn name may fill it, so a 16-byte buffer overflowed the stack.
        char namebuf[sizeof(Task::name)];
        copy_name(namebuf, victim->name, (int)sizeof(namebuf));

        // Same bookkeeping as a normal exit: the status for the parent, the
        // wakeup for whoever is blocked on this id, and the drop of statuses
        // this task itself never collected.
        record_child_exit(victim->parent_id, victim->id, victim->exit_code);
        drop_child_exits_of(victim->id);
        Task* t = ready_queue;
        do {
            if (t->waiting_on == victim->id) {
                t->exit_code_inbox = victim->exit_code;
                t->state = READY;
                t->waiting_on = -1;
                drop_child_exit(victim->id);
            }
            t = t->next;
        } while (t != ready_queue);

        // The victim may be parked in a pipe's waiter list; it can never run
        // again to be woken, and four such ids would fill that pipe's list.
        VFS::forget_task(victim->id);

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
        MemMap::task_gone(victim->id);
        VMM::destroy_user_space(victim->cr3);
        delete[] victim->kstack_mem;
        delete[] victim->fxsave_raw;
        delete victim;

        kprint3("[Sched] Task '", namebuf, "' terminated (killed).\n");
        irq_restore(iflags);
        return 0;
    }

    // Ownership rule for create_user_task: on success the task is linked into
    // ready_queue and owns everything it was given. On -1 nothing was linked,
    // so this function releases the Task, its kernel stack and any user VA
    // reservation itself; 'cr3' and the mappings in it remain the caller's to
    // destroy. A failed attempt still consumes its task id.
    static void discard_unlinked_task(Task* t) {
        delete[] t->kstack_mem;
        delete t;
    }

    int create_user_task(uint64_t rip, const char* name, int argc,
                         const char* const* argv, uint64_t cr3) {
        Task* new_task = new Task;
        if (new_task == nullptr) {
            kprint("[Sched] Out of memory for a task!\n");
            return -1;
        }
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
        new_task->exit_code_inbox = 0;
        new_task->parent_id = current_task->id;
        new_task->zombie_next = nullptr;
        new_task->cr3 = cr3;
        copy_name(new_task->name, name, (int)sizeof(new_task->name));

        // Kernel stack (interrupts from ring 3 land here via the TSS). 32KB
        // so a deep syscall (printf -> PUTS -> kprint -> compositor present)
        // plus a nested IRQ cannot overrun it.
        uint64_t* stack_bottom = new uint64_t[4096];
        if (stack_bottom == nullptr) {
            kprint("[Sched] Out of memory for a kernel stack!\n");
            delete new_task; // no stack yet, so discard_unlinked_task cannot help
            return -1;
        }
        stack_bottom[0] = KSTACK_CANARY; // verified each tick (see sched.cpp)
        uint64_t* stack_top = stack_bottom + 4096;
        new_task->kstack_top = (uint64_t)stack_top;
        new_task->kstack_mem = stack_bottom;

        // User stack: allocated from the per-space VA allocator (no more
        // id-based fixed slots — two processes can overlap freely since
        // they own separate address spaces, and one process can't).
        // One extra page is reserved below it and left unmapped: the stack
        // guard. An overflow faults there instead of silently scribbling.
        uint64_t old_cr3 = VMM::current_pml4();
        bool switched = false;
        if (old_cr3 != new_task->cr3) {
            VMM::switch_pml4(new_task->cr3);
            switched = true;
        }
        uint64_t stack_base = VMM::alloc_user_range(new_task->cr3, USER_STACK_PAGES + 1);
        if (stack_base == 0) {
            kprint("[Sched] Out of user VA for stack!\n");
            if (switched) VMM::switch_pml4(old_cr3);
            // alloc_user_range reserved nothing when it returned 0, so there
            // is no VA range to hand back. stage_spawn destroys the address
            // space; the Task and kernel stack are ours to release here.
            discard_unlinked_task(new_task);
            return -1;
        }
        new_task->user_stack_guard = stack_base; // the unmapped guard page
        // Back every page of the stack: alloc_user_range only reserves the
        // VA range, so the frames must be mapped here. 8 pages (32K) is a
        // comfortable floor; bigger programs put multi-KB frames on it.
        // A failed allocation must not be mapped: physical 0 is reserved
        // memory, and mapping it would let the later space teardown free it.
        // Undo this attempt (mappings and frames), then fail; the Task and
        // kernel stack are ours to release, the address space is the caller's.
        uint64_t mapped = 0;   // pages successfully mapped after the guard
        bool oom = false;
        for (uint64_t p = 0; p < USER_STACK_PAGES; p++) {
            // Zeroed: the frame may still hold another task's data, and the
            // stack below the initial RSP is readable by the new program.
            uint64_t frame = PMM::alloc_page_zeroed();
            if (frame == 0) { oom = true; break; }
            if (!VMM::map_page(frame, stack_base + (p + 1) * 4096, 0x07)) { // P|W|U
                PMM::free_page(frame);
                oom = true;
                break;
            }
            mapped++;
        }
        if (oom) {
            kprint("[Sched] Out of physical memory for user stack!\n");
            // Unmap the guard and every frame that made it, then release the
            // frames themselves.
            VMM::unmap_and_free_user_range(new_task->cr3, stack_base + 4096, mapped);
            VMM::unmap_user_range(new_task->cr3, stack_base, 1); // guard marker
            if (switched) VMM::switch_pml4(old_cr3);
            // The guard marker and the frames above it are gone, so no VA
            // reservation is left for the caller. Only the Task and its
            // kernel stack still need releasing.
            discard_unlinked_task(new_task);
            return -1;
        }
        if (switched) VMM::switch_pml4(old_cr3);

        // --- Build the initial user stack (System V layout) ---
        // [rsp]      = argc
        // [rsp+8..]  = argv[0..argc-1], then NULL
        // then       = envp[0..envc-1], then NULL
        // higher     = the argv and env string bytes themselves
        //
        // crt0 finds the environment as argv + argc + 1, which is why the two
        // arrays are laid out together rather than pointed at separately.
        static uint64_t str_addrs[8];
        static uint64_t env_addrs[Env::MAX_HANDOFF];
        int envc = Env::count();
        if (envc > Env::MAX_HANDOFF) envc = Env::MAX_HANDOFF;

        uint64_t total_len = 0;
        for (int i = 0; i < argc; i++) {
            const char* s = argv[i];
            while (*s) { total_len++; s++; }
            total_len++; // NUL
        }
        for (int i = 0; i < envc; i++) {
            const char* s = Env::at(i);
            while (*s) { total_len++; s++; }
            total_len++;
        }
        uint64_t stack_top_va = stack_base + (USER_STACK_PAGES + 1) * 4096;
        uint64_t strings_start = (stack_top_va - total_len) & ~(uint64_t)15;
        // Align the pointer array down so the initial RSP is 16n+8: the SysV
        // convention a called function expects after its return address is
        // pushed. SSE spills need that alignment.
        uint64_t nptrs = (uint64_t)(argc + 1) + (uint64_t)(envc + 1);
        uint64_t array_start = (strings_start - nptrs * 8) & ~(uint64_t)15;
        uint64_t new_rsp = array_start - 8;

        char* dst = (char*)strings_start;
        for (int i = 0; i < argc; i++) {
            str_addrs[i] = (uint64_t)dst;
            const char* s = argv[i];
            while (*s) *dst++ = *s++;
            *dst++ = '\0';
        }
        for (int i = 0; i < envc; i++) {
            env_addrs[i] = (uint64_t)dst;
            const char* s = Env::at(i);
            while (*s) *dst++ = *s++;
            *dst++ = '\0';
        }
        uint64_t* ap = (uint64_t*)array_start;
        for (int i = 0; i < argc; i++) ap[i] = str_addrs[i];
        ap[argc] = 0;                                       // argv[argc] = NULL
        for (int i = 0; i < envc; i++) ap[argc + 1 + i] = env_addrs[i];
        ap[argc + 1 + envc] = 0;                            // envp[envc] = NULL
        *(uint64_t*)new_rsp = (uint64_t)argc;

        new_task->user_rip = rip;
        new_task->user_rsp = new_rsp;
        new_task->brk = 0; // 0 = unset; brk syscall initializes on first use
        new_task->brk_limit = 0;
        new_task->fb_map_base = 0;  // mapped on the first FBINFO call
        new_task->fb_map_pages = 0;
        // CWD is inherited from the spawning process (Unix semantics) —
        // otherwise `cd /bin` + `ls` would list "/" in every child. The copy is
        // bounded like the setter: a path cannot be longer than the field.
        int ci = 0;
        while (current_task->cwd[ci] && ci < (int)sizeof(new_task->cwd) - 1) {
            new_task->cwd[ci] = current_task->cwd[ci];
            ci++;
        }
        new_task->cwd[ci] = '\0';

        // FPU area: separately allocated (heap is only 8-aligned; fxsave
        // needs 16) and seeded with the boot-clean template so the first
        // fxrstor is valid. Failure here is a failed spawn like any other: the
        // user stack is already mapped and reserved, so it is handed back the
        // way the out-of-memory path above does it. The copy below must never
        // run through a null pointer.
        new_task->fxsave_raw = new uint8_t[512 + 63];
        if (new_task->fxsave_raw == nullptr) {
            kprint("[Sched] Out of memory for the FPU area!\n");
            VMM::unmap_and_free_user_range(new_task->cr3, stack_base + 4096, mapped);
            VMM::unmap_user_range(new_task->cr3, stack_base, 1); // guard marker
            discard_unlinked_task(new_task);
            return -1;
        }
        new_task->fxsave_area = (uint8_t*)(((uint64_t)new_task->fxsave_raw + 63) & ~(uint64_t)63);
        for (int i = 0; i < 512; i++) new_task->fxsave_area[i] = fpu_template[i];
        // The child inherits the parent's descriptors (Unix semantics): its
        // stdout is where the parent pointed fd 1, which is what makes a shell
        // redirection or a pipe end survive the spawn. Console entries share the
        // one console endpoint.
        VFS::fds_inherit(new_task->fds);

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

        kprint3("[Sched] User task '", name, "' created.\n");
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
    // the caller via VFS), then store the canonical path. The field is
    // VFS::PATH_MAX, the same limit canonicalize enforces, so a path the VFS
    // accepted cannot be cut down here.
    void current_task_set_cwd(const char* path) {
        int i = 0;
        while (path[i] && i < (int)sizeof(current_task->cwd) - 1) {
            current_task->cwd[i] = path[i];
            i++;
        }
        current_task->cwd[i] = '\0';
    }

    // Queue an exited task for the next switch to free. A single slot would
    // lose the older task if two exit before any survivor resumes, leaking
    // its kernel stack, FPU area and address space for good.
    static void queue_zombie(Task* dead) {
        dead->zombie_next = nullptr;
        if (zombie_tail != nullptr) zombie_tail->zombie_next = dead;
        else zombie_head = dead;
        zombie_tail = dead;
    }

    void exit_current(uint64_t code) {
        Task* dead = current_task;
        dead->exit_code = code;
        kprint3("[Sched] Task '", dead->name, "' exited.\n");

        // 1. Record this exit where only the parent can collect it. A waiter
        //    already blocked on this id takes the code directly below.
        record_child_exit(dead->parent_id, dead->id, dead->exit_code);
        drop_child_exits_of(dead->id);

        // 2. Wake anyone blocked on this task (and give them the code).
        //    A waking waiter consumes the record: the deliverable is the inbox
        //    now, and WAIT on this id must not resolve twice.
        Task* t = ready_queue;
        do {
            if (t->waiting_on == dead->id) {
                t->exit_code_inbox = dead->exit_code;
                t->state = READY;
                t->waiting_on = -1;
                drop_child_exit(dead->id);
            }
            t = t->next;
        } while (t != ready_queue);

        // 3. Last task standing? Nothing to switch to — halt forever.
        if (dead->next == dead) {
            kprint("[Sched] No tasks left. Halting.\n");
            while (1) asm("hlt");
        }

        // 4. Unlink from the circular list
        // The task may be parked in a pipe's waiter list and will never run
        // again to be woken, so its ids must go before the slot can be reused.
        VFS::forget_task(dead->id);

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

        // 6. Queue the Task struct for cleanup AFTER the next switch (we are
        //    standing on the dying kernel stack, so we can't free it here).
        queue_zombie(dead);

        current_task = next;
        GDT::tss.rsp0 = next->kstack_top;
        switch_to_task_cr3(next);
        uint64_t dummy_rsp; // never used — this task ceases to exist
        switch_context(&dummy_rsp, next->rsp, dead->fxsave_area, next->fxsave_area);
        // Never returns: the CPU is now on the next task's stack.
        while (1) asm("hlt");
    }

} // namespace Sched

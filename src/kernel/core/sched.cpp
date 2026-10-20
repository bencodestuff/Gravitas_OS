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
#include "kernel/core/memmap.h"
#include "kernel/core/input.h"
#include "kernel/dev/keyboard.h"

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
    Task* zombie_head = nullptr; // exited tasks awaiting cleanup by the next switch
    Task* zombie_tail = nullptr;

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
        current_task->getc_waiting = false;
        current_task->event_input = false;
        current_task->console_owner = -1;
        current_task->con_in_head = current_task->con_in_tail = 0;
        current_task->con_out_head = current_task->con_out_tail = 0;
        current_task->con_notified = false;
        current_task->timer_deadline = 0;
        current_task->timer_armed = false;
        current_task->user_rip = 0;
        current_task->user_rsp = 0;
        current_task->user_stack_guard = 0;
        current_task->kstack_mem = nullptr; // the boot stack has no canary
        current_task->cwd[0] = '/'; current_task->cwd[1] = '\0';
        VFS::fds_init_current();   // 0/1/2 = the console (the boot task's stdio)
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
                    t->wake_timestamp == (uint64_t)-1 && !t->getc_waiting) {
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
            out[n].name[15] = '\0'; // TaskInfo::name is 16 bytes; truncate
            n++;
            t = t->next;
        } while (t != ready_queue);
        return n;
    }

    // --- Keyboard focus ---
    // Which task GETC delivers to. -1 = no focus policy: any waiting task
    // is eligible (the pre-WM behavior).
    static int focus_task = -1;

    void block_on_keyboard() {
        current_task->getc_waiting = true;
        current_task->state = SLEEPING;
        current_task->wake_timestamp = (uint64_t)-1; // only the IRQ wakes us
    }

    void clear_keyboard_waiting() {
        current_task->getc_waiting = false;
    }

    // Wake the task a key should go to: the focused one, or (with no focus
    // set) the oldest waiting task — the console. getc_waiting is NOT
    // cleared here — the woken task clears it once it actually drains a
    // key, so a wake with an empty queue simply re-blocks.
    void wake_keyboard_waiter() {
        Task* t = ready_queue;
        if (t == nullptr) return;

        if (focus_task != -1) {
            // The foreground job owns the console: when the focused task is
            // blocked on a child — the shell running `lua`, say — the child is
            // the reader, so the key goes to it rather than to the parent
            // (which is not reading; it is waiting for the child to exit).
            int reader = focus_task;
            Task* f = ready_queue;
            do {
                if (f->id == focus_task) {
                    if (f->waiting_on >= 0) reader = f->waiting_on;
                    break;
                }
                f = f->next;
            } while (f != ready_queue);

            do {
                if (t->getc_waiting && t->id == reader) {
                    t->state = READY;
                    return;
                }
                t = t->next;
            } while (t != ready_queue);
            return;
        }

        // No focus: pick the lowest-id waiter (the console/shell, spawned
        // first). Without a rule like this, several GETC waiters would race
        // and the recipient would depend on queue order.
        Task* best = nullptr;
        do {
            if (t->getc_waiting && (best == nullptr || t->id < best->id)) best = t;
            t = t->next;
        } while (t != ready_queue);
        if (best != nullptr) best->state = READY;
    }

    void set_input_focus(int task_id) {
        focus_task = task_id;
        wake_keyboard_waiter(); // a queued key may now reach the new focus
    }

    int input_focus() { return focus_task; }

    void input_focus_gone(int task_id) {
        if (focus_task == task_id) {
            focus_task = -1;
            wake_keyboard_waiter();
        }
    }

    void set_event_input(bool on) { current_task->event_input = on; }

    bool task_event_input(int task_id) {
        Task* t = ready_queue;
        if (t == nullptr) return false;
        do {
            if (t->id == task_id) return t->event_input;
            t = t->next;
        } while (t != ready_queue);
        return false;
    }

    // --- Console endpoint (M12) ---
    //
    // A bound task's PUTC bytes go to its owner's out ring; the owner's
    // CONSOLE_WRITE bytes go to the bound task's in ring (its GETC). Both
    // sides touch the rings from syscall context, so every mutation is
    // guarded with cli (save/restore, not a bare sti — GETC holds IF off
    // across its loop).

    static inline uint64_t irq_save() {
        uint64_t f;
        asm volatile("pushfq; popq %0; cli" : "=r"(f) :: "memory");
        return f;
    }
    static inline void irq_restore(uint64_t f) {
        if (f & (1ull << 9)) asm volatile("sti"); else asm volatile("cli");
    }

    static Task* find_task(int id) {
        if (ready_queue == nullptr) return nullptr;
        Task* t = ready_queue;
        do {
            if (t->id == id) return t;
            t = t->next;
        } while (t != ready_queue);
        return nullptr;
    }

    // Ctrl+C. A task blocked on a child (a foreground SPAWN or WAIT) carries
    // that child in waiting_on; killing the child wakes the blocked task, which
    // is exactly "interrupt the foreground job". The exit code is the shell
    // convention 128 + SIGINT(2).
    bool interrupt_foreground(int task_id) {
        if (task_id < 0) return false;
        Task* t = find_task(task_id);
        if (t == nullptr || t->waiting_on < 0) return false;

        int child = t->waiting_on;

        // The victim is usually the running task: the parent is blocked, so the
        // scheduler is on the child. kill_task refuses to kill current_task, and
        // its synchronous teardown would free the stack we are standing on —
        // go through the exit path instead, which switches away and lets the
        // scheduler reap it.
        if (current_task != nullptr && child == current_task->id) {
            exit_current(130);
            return true;
        }
        return kill_task(child, 130) == 0;
    }

    int console_owner_of_current() {
        return current_task ? current_task->console_owner : -1;
    }

    bool console_attach(int task_id) {
        if (task_id <= 0 || task_id == current_task->id) return false;
        Task* t = find_task(task_id);
        if (t == nullptr || t->console_owner != -1) return false;
        t->console_owner = current_task->id;
        return true;
    }

    void inherit_console(int child_id) {
        Task* c = find_task(child_id);
        if (c != nullptr) c->console_owner = current_task->console_owner;
    }

    bool console_detach(int task_id) {
        Task* t = find_task(task_id);
        if (t == nullptr || t->console_owner != current_task->id) return false;
        t->console_owner = -1;
        return true;
    }

    void console_owner_gone(int owner_id) {
        if (ready_queue == nullptr) return;
        Task* t = ready_queue;
        do {
            if (t->console_owner == owner_id) t->console_owner = -1;
            t = t->next;
        } while (t != ready_queue);
    }

    // One coalesced wakeup so a terminal blocked in RECV learns output
    // arrived. The message carries no payload: the byte stream lives in the
    // ring, this is only the nudge (and one at a time — see con_notified).
    static void console_notify(Task* owner) {
        if (owner->con_notified) return;
        Ipc::Msg m;
        m.from = (uint64_t)-1;
        m.type = Input::EV_CONSOLE_OUT;
        m.a = 0;
        m.b = 0;
        if (Ipc::send(owner->id, m) == 0) owner->con_notified = true;
    }

    static void console_out_push(Task* owner, char c) {
        int next = (owner->con_out_head + 1) % CONSOLE_RING;
        if (next == owner->con_out_tail) return; // full: drop (a full tty)
        owner->con_out[owner->con_out_head] = c;
        owner->con_out_head = next;
        console_notify(owner);
    }

    void console_emit(char c) {
        int owner_id = current_task->console_owner;
        if (owner_id == -1) {
            char s[2] = { c, 0 };
            kprint(s);
            return;
        }
        uint64_t f = irq_save();
        Task* owner = find_task(owner_id);
        if (owner != nullptr) {
            console_out_push(owner, c);
            irq_restore(f);
            return;
        }
        current_task->console_owner = -1; // owner died: fall back to kprint
        irq_restore(f);
        char s[2] = { c, 0 };
        kprint(s);
    }

    // A whole run of bytes under the same routing: into the owner's ring if
    // bound, else the kernel console. The kernel console is kprinted in chunks
    // (kprint is atomic per call), so the bytes need not be NUL-terminated —
    // which matters for a raw write(1) whose buffer is not C text.
    void console_emit_bytes(const char* s, int n) {
        int owner_id = current_task->console_owner;
        if (owner_id == -1) {
            char chunk[256];
            while (n > 0) {
                int take = n < 255 ? n : 255;
                for (int i = 0; i < take; i++) chunk[i] = s[i];
                chunk[take] = '\0';
                kprint(chunk);
                s += take;
                n -= take;
            }
            return;
        }
        uint64_t f = irq_save();
        Task* owner = find_task(owner_id);
        if (owner != nullptr) {
            for (int i = 0; i < n; i++) console_out_push(owner, s[i]);
            irq_restore(f);
            return;
        }
        current_task->console_owner = -1;   // owner died: fall back to kprint
        irq_restore(f);
        console_emit_bytes(s, n);
    }

    // PUTS: the string spelling of the same routing.
    void console_emit_str(const char* s, int n) { console_emit_bytes(s, n); }

    int console_read(char* buf, int max) {
        if (buf == nullptr || max <= 0) return 0;
        Task* me = current_task;
        int n = 0;
        uint64_t f = irq_save();
        while (n < max && me->con_out_tail != me->con_out_head) {
            buf[n++] = me->con_out[me->con_out_tail];
            me->con_out_tail = (me->con_out_tail + 1) % CONSOLE_RING;
        }
        me->con_notified = false; // the queued wakeup is spent
        bool more = (me->con_out_tail != me->con_out_head);
        if (more) console_notify(me); // keep one armed while data remains
        irq_restore(f);
        return n;
    }

    int console_write(int task_id, const char* buf, int n) {
        if (buf == nullptr || n <= 0) return 0;
        Task* t = find_task(task_id);
        if (t == nullptr || t->console_owner != current_task->id) return -1;

        // The foreground job is the reader: a hosted shell delegates to the
        // child it is blocked on (running `lua`), so its input goes to the
        // child's ring rather than the shell's.
        Task* dst = t;
        if (t->waiting_on >= 0) {
            Task* c = find_task(t->waiting_on);
            if (c != nullptr) dst = c;
        }

        int written = 0;
        uint64_t f = irq_save();
        for (int i = 0; i < n; i++) {
            // Ctrl+C in a hosted console's input is the interrupt key: it kills
            // the hosted task's foreground job instead of being queued as input
            // (so a terminal that forwards keys makes Ctrl+C work end to end).
            if (buf[i] == 0x03) { interrupt_foreground(task_id); continue; }
            int next = (dst->con_in_head + 1) % CONSOLE_RING;
            if (next == dst->con_in_tail) break; // full: stop
            dst->con_in[dst->con_in_head] = buf[i];
            dst->con_in_head = next;
            written++;
        }
        if (written > 0 && dst->getc_waiting) dst->state = READY;
        irq_restore(f);
        return written;
    }

    int console_ring_getc() {
        Task* me = current_task;
        uint64_t f = irq_save();
        if (me->con_in_tail == me->con_in_head) {
            irq_restore(f);
            return -1;
        }
        int c = (unsigned char)me->con_in[me->con_in_tail];
        me->con_in_tail = (me->con_in_tail + 1) % CONSOLE_RING;
        irq_restore(f);
        return c;
    }

    // GETC: block until one byte is available — from the console's in-ring when
    // this task is bound to a terminal, otherwise the keyboard. The owner is
    // re-read every pass, so a task bound while already blocked switches rings.
    // Interrupts stay off across the check-and-block so a byte landing in
    // between cannot be missed. No echo: the raw reader echoes for itself (the
    // console descriptor's line read in the VFS does).
    int console_getc_raw() {
        asm volatile("cli");
        int b = -1;
        while (true) {
            if (current_task->console_owner != -1) {
                b = console_ring_getc();
                if (b >= 0) break;
            } else {
                char kc = Keyboard::get_queued_key();
                if (kc != 0) { b = (int)kc; break; }
            }
            block_on_keyboard();
            yield();   // returns when a key/write wakes us
        }
        clear_keyboard_waiting();
        return b;
    }

    // --- Timed wakeup (userland) ---
    void timer_arm_current(uint64_t ms) {
        if (ms == 0) { current_task->timer_armed = false; return; }
        uint64_t t = (ms + 19) / 20; // 50Hz -> 20ms/tick, round up
        if (t == 0) t = 1;
        current_task->timer_deadline = Timer::get_ticks() + t;
        current_task->timer_armed = true;
    }

    void timer_tick() {
        if (ready_queue == nullptr) return;
        uint64_t now = Timer::get_ticks();
        Task* t = ready_queue;
        do {
            if (t->timer_armed && now >= t->timer_deadline) {
                Ipc::Msg m;
                m.from = (uint64_t)-1;
                m.type = Input::EV_TIMER;
                m.a = 0;
                m.b = 0;
                // Keep it armed if the mailbox is full, so the wakeup is not
                // lost (retried next tick).
                if (Ipc::send(t->id, m) == 0) t->timer_armed = false;
            }
            t = t->next;
        } while (t != ready_queue);
    }

    // --- Stack guards ---
    uint64_t current_task_stack_guard() {
        return current_task ? current_task->user_stack_guard : 0;
    }

    void check_kernel_stack() {
        if (current_task == nullptr || current_task->kstack_mem == nullptr) return;
        if (current_task->kstack_mem[0] != KSTACK_CANARY) {
            kprint("\n[sched] KERNEL STACK OVERFLOW in task '");
            kprint(current_task->name);
            kprint("' — halting (this is a kernel bug)\n");
            asm volatile("cli; hlt");
            for (;;) asm("hlt");
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
        // Reap every task that exited since the last switch (its old kernel
        // stack is definitely abandoned by now). A queue, not one slot: two
        // tasks can exit before any survivor resumes, and each owns a kernel
        // stack, an FPU area and a whole address space. CLI guards the list:
        // a nested PIT yield could otherwise reap on our behalf and advance
        // the head mid-loop.
        asm volatile("cli");
        while (zombie_head != nullptr) {
            Task* dead = zombie_head;
            zombie_head = dead->zombie_next;
            if (zombie_head == nullptr) zombie_tail = nullptr;
            // Close any fds the task leaked, then free its address space
            for (int i = 0; i < VFS::MAX_FDS; i++) {
                if (dead->fds[i] != nullptr) {
                    VFS::close_fd_for(dead->fds, i);
                }
            }
            Compositor::surfaces_for_task_gone(dead->id);
            Compositor::authority_gone(dead->id);
            input_focus_gone(dead->id);
            console_owner_gone(dead->id);
            Ipc::task_gone(dead->id);
            MemMap::task_gone(dead->id);
            // Free the exited task's address space (its user frames + tables)
            VMM::destroy_user_space(dead->cr3);
            delete[] dead->kstack_mem;
            delete[] dead->fxsave_raw;
            delete dead;
        }
        asm volatile("sti");
    }

} // namespace Sched

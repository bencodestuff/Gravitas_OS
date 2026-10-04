#include "idt.h"
#include "pic.h"
#include "kernel/io.h"
#include "kernel/dev/keyboard.h"
#include "kernel/core/fault.h"

#include "kernel/core/timer.h"
#include "kernel/core/sched.h"
#include "kernel/core/ipc.h"
#include "kernel/dev/mouse.h"
#include "kernel/gfx/compositor.h"
#include "kernel/gfx/framebuffer.h"
#include "kernel/core/vmm.h"
#include "fs/tar.h"
#include "fs/elf.h"
#include "fs/vfs.h"
#include "kernel/core/pmm.h"
#include "kernel/core/sysinfo.h"
// brk arena: every process gets the same start VA (they live in separate
// address spaces now), kept far away from code (0x400000+) and stacks
// (0x500000+). Grows upward in whole pages.
#define BRK_BASE 0x8000000

// Boot mounts the tarfs here; bare filenames resolve through it (embryonic
// PATH: "ls.elf" -> /bin/ls.elf)
#define BIN_MOUNT "/bin"

// chdir/getcwd: per-process CWD, lives on the Task
#define CWD_MAX 64

extern "C" void kprint(const char* str);
extern "C" void kprint_uint(uint64_t value); 

// Hex printer for registers (no allocation, no format string needed)
static void print_hex64(uint64_t value) {
    kprint("0x");
    for (int i = 60; i >= 0; i -= 4) {
        uint64_t n = (value >> i) & 0xF;
        char c = (n < 10) ? '0' + n : 'A' + (n - 10);
        char s[2] = {c, '\0'};
        kprint(s);
    }
}

// Kernel-side staging buffer for spawned program names (switching CR3
// invalidates user pointers, so the name is copied before any load).
static char spawn_namebuf[64];

// Link to the assembly table
extern "C" void* isr_stub_table[];
// Explicit syscall stub (vector 128)
extern "C" void isr_128();

namespace IDT {

    __attribute__((aligned(0x10)))
    static IDTEntry idt[256];
    static IDTR idtr;

    void set_gate(int interrupt_num, uint64_t handler_addr, uint8_t ist, uint8_t type_attr) {
        idt[interrupt_num].offset_low  = handler_addr & 0xFFFF;
        idt[interrupt_num].selector    = 0x08; 
        idt[interrupt_num].ist         = ist;
        idt[interrupt_num].attributes  = type_attr;
        idt[interrupt_num].offset_mid  = (handler_addr >> 16) & 0xFFFF;
        idt[interrupt_num].offset_high = (handler_addr >> 32) & 0xFFFFFFFF;
        idt[interrupt_num].reserved    = 0;
    }

    void flush_keyboard() {
        // Read status register (0x64)
        // If bit 0 (Output Buffer Full) is set, read the data port (0x60) to burn it.
        while (inb(0x64) & 1) {
            inb(0x60);
        }
    }

    void init() {
        kprint("[IDT] Initializing...\n");

        idtr.limit = sizeof(idt) - 1;
        idtr.base  = (uint64_t)&idt;

        // Fill the first 48 entries (CPU Exceptions + IRQs)
        // 0x8E = Present(1) | Ring0(00) | Gate(1110 = Interrupt Gate)
            for (int i = 0; i < 48; i++) {
                // IST=0 for everything: handlers run on the current task's
                // kernel stack (TSS.rsp0, kept up to date by the scheduler).
                // A shared IST stack cannot be used for anything that blocks
                // or switches tasks — the next interrupt on the same IST
                // would stomp the saved context of the task we switched away
                // from (this exact bug cost us a day).
                uint8_t ist = 0;
                set_gate(i, (uint64_t)isr_stub_table[i], ist, 0x8E);
        }

        // Syscall gate: trap gate (0xEE, IF stays enabled), IST=0 so each
        // task's syscalls — including blocking ones like spawn — live on
        // its own kernel stack.
        set_gate(0x80, (uint64_t)isr_128, 0, 0xEE);

        kprint("[IDT] Loading IDT...\n");
        asm volatile ("lidt %0" : : "m"(idtr)); // Load it!
        kprint("[IDT] Loaded.\n");
        
        // FLUSH KEYBOARD BEFORE ENABLING INTERRUPTS
        flush_keyboard();

        // Enable interrupts!
        asm volatile ("sti"); 
        kprint("[IDT] Interrupts Enabled.\n");
    }
}

// Stages a spawn: kernel-side copies of argv, VFS read of the ELF image,
// child address-space clone + load + task creation. Returns the child id,
// or -1. Shared by SPAWN (4, foreground) and SPAWN_BG (21, immediate).
static int stage_spawn(IDT::Registers* regs, const char* staged_name) {
    // Stage argv into kernel memory: the kernel is about to switch CR3,
    // so user pointers may not survive the swap.
    static char argv_stage[8][64];
    const char* argv_copy[8];
    uint64_t user_argc = regs->rdx;
    if (user_argc > 8) user_argc = 8;
    const char** user_argv = (const char**)regs->rsi;
    int argc = 0;
    for (uint64_t i = 0; i < user_argc; i++) {
        const char* us = user_argv[i];
        if (us == nullptr) break;
        int ci = 0;
        while (ci < 63 && us[ci] != '\0') {
            argv_stage[i][ci] = us[ci];
            ci++;
        }
        argv_stage[i][ci] = '\0';
        argv_copy[i] = argv_stage[i];
        argc = i + 1;
    }

    // Read the program's ELF image through the VFS (bare names resolve
    // through /bin). Heap buffer, freed after the load.
    int fd = VFS::open_fd(staged_name, VFS::O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    uint64_t fsize = VFS::fd_size(fd);
    char* data = new char[fsize];
    long got = VFS::read_fd(fd, data, fsize);
    VFS::close_fd(fd);
    if (got != (long)fsize) {
        delete[] data;
        return -1;
    }

    // Build the child in its OWN address space: clone, switch, load the
    // ELF there, create the task, switch back.
    uint64_t old_cr3 = VMM::current_pml4();
    uint64_t child_cr3 = VMM::clone_kernel_space();
    VMM::switch_pml4(child_cr3);

    uint64_t entry = ELF::load_elf((uint8_t*)data);
    delete[] data;
    if (entry == 0) {
        VMM::switch_pml4(old_cr3);
        VMM::destroy_user_space(child_cr3);
        return -1;
    }

    // create_user_task switches to the child space itself and stages the
    // kernel-side argv strings onto the child's initial user stack.
    int child = Sched::create_user_task(entry, staged_name, argc, argv_copy, child_cr3);
    VMM::switch_pml4(old_cr3);
    if (child < 0) {
        VMM::destroy_user_space(child_cr3);
        return -1;
    }
    return child;
}

// --- THE C++ HANDLER ---
// This is called by Assembly
extern "C" void interrupt_dispatcher(IDT::Registers* regs) {
    uint64_t num = regs->interrupt_number;

    if (num == 0x80) {
        uint64_t syscall_id = regs->rax;
        uint64_t arg1 = regs->rdi;

        if (syscall_id == 0) { // WRITE (raw char to screen + serial)
             char c = (char)arg1;
             char str[2] = {c, 0};
             kprint(str);
        }
        else if (syscall_id == 1) { // READ (Keyboard) — blocks until a key
             // Interrupts off around the check-and-block so a key arriving
             // in between can't be missed (the IRQ will wake us instead).
             asm volatile("cli");
             char c = 0;
             while (true) {
                 c = Keyboard::get_queued_key();
                 if (c != 0) break;
                 Sched::block_on_keyboard();
                 Sched::yield(); // returns when the keyboard IRQ wakes us
             }
             regs->rax = (uint64_t)c;
        }
        /* Syscall 2 (list tarfs) and 3 (read by name) RETIRED — superseded by
   the VFS: open/read/close fds (7-10) and readdir (11). The numbers are
   deliberately unused holes, like real OSes keep. */
        else if (syscall_id == 11) { // LISTDIR: rdi = dir path, rsi = buf, rdx = max
             VFS::readdir((const char*)regs->rdi, (char*)regs->rsi, regs->rdx);
             regs->rax = 0;
        }
        else if (syscall_id == 12) { // CHDIR: rdi = path -> rax = 0 or -1
             const char* path = (const char*)regs->rdi;



             // Must exist and be a directory
             if (!VFS::is_directory(path)) {
                 regs->rax = (uint64_t)-1;
             } else {
                 // Store the canonical path (cwd-relative + ./.. resolved)
                 static char canon[64];
                 VFS::canonicalize(path, canon, sizeof(canon));
                 Sched::current_task_set_cwd(canon);
                 regs->rax = 0;
             }
        }
        else if (syscall_id == 13) { // GETCWD: rdi = buf, rsi = max
             char* buf = (char*)regs->rdi;
             const char* cwd = Sched::current_task_cwd();
             uint64_t max = regs->rsi;
             uint64_t i = 0;
             while (cwd[i] && i + 1 < max) { buf[i] = cwd[i]; i++; }
             buf[i] = '\0';
             regs->rax = (uint64_t)i;
        }
        else if (syscall_id == 14) { // SLEEP: rdi = ms — block, wake on timer
             // Timer ticks at 50Hz (20ms per tick). Round up so sleep(1)
             // actually sleeps one tick instead of returning instantly.
             uint64_t ms = regs->rdi;
             uint64_t ticks = (ms + 19) / 20;
             Sched::sleep_current(ticks);
             // Record the sleep intent NOW and switch away at once: without
             // the yield, this task would keep running (with its repeat
             // syscall returns) until some later PIT tick bounces it off.
             asm volatile("cli");
             Sched::yield();
        }
        else if (syscall_id == 15) { // MOUSE READ: rdi = struct* {dx,dy,buttons}
             // Non-blocking. rax = 1 if a packet was drained, 0 if none.
             struct MousePkt { int16_t dx, dy; uint8_t buttons; };
             MousePkt* out = (MousePkt*)regs->rdi;
             int16_t dx, dy; uint8_t buttons;
             if (Mouse::read_packet(&dx, &dy, &buttons)) {
                 out->dx = dx; out->dy = dy; out->buttons = buttons;
                 regs->rax = 1;
             } else {
                 regs->rax = 0;
             }
        }
        else if (syscall_id == 16) { // FB INFO+MAP: rdi = grv_fb_info*
             // Maps the framebuffer into the CALLING task's address space
             // at a fresh user VA, returns geometry + that address.
             struct FbPkt { uint64_t w,h,pitch,bpp,address,size; };
             FbPkt* out = (FbPkt*)regs->rdi;
             Framebuffer::Info info;
             if (Framebuffer::get_info_and_map(Sched::current_task_cr3(), &info)) {
                 out->w = info.width; out->h = info.height;
                 out->pitch = info.pitch; out->bpp = info.bpp;
                 out->address = info.address; out->size = info.size;
                 regs->rax = 0;
             } else {
                 regs->rax = (uint64_t)-1;
             }
        }
        else if (syscall_id == 17) { // TICKS: timer ticks since boot (50Hz)
             regs->rax = Timer::get_ticks();
        }
        else if (syscall_id == 18) { // SURFACE CREATE: rdi=w, rsi=h, rdx=out struct
             long id = Compositor::surface_create(regs->rdi, regs->rsi);
             if (id >= 0 && regs->rdx) {
                 Compositor::Info info;
                 if (Compositor::surface_info((int)id, &info))
                     *(Compositor::Info*)regs->rdx = info;
             }
             regs->rax = (uint64_t)id;
        }
        else if (syscall_id == 19) { // SURFACE COMMIT: repaint our surfaces
             Compositor::surface_commit();
             regs->rax = 0;
        }
        else if (syscall_id == 20) { // SURFACE MOVE: rdi=id, rsi=x, rdx=y
             Compositor::surface_move((int)regs->rdi, (int)regs->rsi, (int)regs->rdx);
             regs->rax = 0;
        }
        else if (syscall_id == 26) { // SURFACE DESTROY: rdi=id -> 0 or -1
             regs->rax = (uint64_t)(long)Compositor::surface_destroy((int)regs->rdi);
        }
        else if (syscall_id == 27) { // SURFACE Z: rdi=id, rsi=pos -> 0 or -1
             regs->rax = (uint64_t)(long)Compositor::surface_z((int)regs->rdi, (int64_t)regs->rsi);
        }
        else if (syscall_id == 28) { // SURFACE LIST: rdi=Info*, rsi=max -> count
             regs->rax = (uint64_t)(long)Compositor::surface_list(
                 (Compositor::Info*)regs->rdi, regs->rsi);
        }
        else if (syscall_id == 29) { // SYSINFO: rdi = SysInfo* -> 0
             Sysinfo::get((SysInfo*)regs->rdi);
             regs->rax = 0;
        }
        else if (syscall_id == 30) { // TASK LIST: rdi=TaskInfo*, rsi=max -> count
             regs->rax = (uint64_t)(long)Sched::task_snapshot(
                 (Sched::TaskInfo*)regs->rdi, (int)regs->rsi);
        }
        else if (syscall_id == 31) { // FAULT LAST: rdi = Fault::Record* -> 1/0
             regs->rax = Fault::last_fault((Fault::Record*)regs->rdi) ? 1 : 0;
        }
        else if (syscall_id == 4 || syscall_id == 21) { // SPAWN / SPAWN_BG:
             // Blocks until the spawned program exits — EXCEPT SPAWN_BG (21)
             // which returns immediately. rax = child task id, or -1.
             const char* user_name = (const char*)regs->rdi;
             if (user_name == nullptr) { regs->rax = (uint64_t)-1; return; }

             // Stage the name into kernel memory: the kernel is about to
             // switch CR3, so user pointers may not survive the swap.
             // (Terminate at the true end: the buffer is reused across
             // spawns, and stale chars from longer names could leak.)
             int ni = 0;
             while (ni < 63 && user_name[ni] != '\0') {
                 spawn_namebuf[ni] = user_name[ni];
                 ni++;
             }
             spawn_namebuf[ni] = '\0';

             int child = stage_spawn(regs, spawn_namebuf);
             if (child < 0) { regs->rax = (uint64_t)-1; return; }

             if (syscall_id == 4) {
                 // Foreground: wait for the child
                 asm volatile("cli");
                 Sched::block_current(child);
                 Sched::yield(); // returns when the child wakes us
             }
             regs->rax = (uint64_t)child;
        }
        else if (syscall_id == 5) { // EXIT: kill the calling task. Never returns.
             asm volatile("cli");
             Sched::exit_current(regs->rdi); // code from RDI — stored for waitpid
        }
        else if (syscall_id == 22) { // WAIT: rdi = task id -> rax = exit code
             long id = (long)regs->rdi;
             uint64_t code = 0;
             if (id <= 0) { regs->rax = (uint64_t)-1; }
             else if (Sched::consume_recent_exit((int)id, &code)) {
                 regs->rax = code;
             } else if (Sched::task_exists((int)id)) {
                 // live task: block until it exits (exit_current fills our inbox)
                 asm volatile("cli");
                 Sched::block_current((int)id);
                 Sched::yield();
                 regs->rax = Sched::current_exit_inbox();
             } else {
                 regs->rax = (uint64_t)-1; // unknown task (never existed / ring overflow)
             }
        }
        else if (syscall_id == 23) { // KILL: rdi = id, rsi = exit code
             regs->rax = (uint64_t)Sched::kill_task((int)regs->rdi, regs->rsi);
        }
        else if (syscall_id == 24) { // SEND: rdi = dst id, rsi = user Msg*
             // The kernel stamps 'from' with OUR id — senders cannot pose
             // as someone else, and receivers can trust the field.
             const Ipc::Msg* um = (const Ipc::Msg*)regs->rsi;
             if (um == nullptr) { regs->rax = (uint64_t)-1; return; }
             Ipc::Msg m;
             m.from  = Sched::current_task_id();
             m.type  = um->type;   // user-VA reads run on the caller's CR3
             m.a     = um->a;      // (house pattern: see FBINFO)
             m.b     = um->b;
             regs->rax = (uint64_t)Ipc::send((int)regs->rdi, m);
        }
        else if (syscall_id == 25) { // RECV: rdi = user Msg*, rsi = block flag
             // Blocking form: the cli-guarded check-and-block loop is the
             // standard discipline (spawn/wait use it too). A send between
             // our check and our waiter registration is impossible with
             // IF off; one after registration wakes us via wake_task.
             Ipc::Msg* um = (Ipc::Msg*)regs->rdi;
             if (um == nullptr) { regs->rax = (uint64_t)-1; return; }
             while (true) {
                 asm volatile("cli");
                 Ipc::Msg m;
                 int r = Ipc::try_recv(&m);
                 if (r == 0) {
                     asm volatile("sti");
                     um->from = m.from;
                     um->type = m.type;
                     um->a    = m.a;
                     um->b    = m.b;
                     regs->rax = 0;
                     return;
                 }
                 if (regs->rsi == 0) {
                     asm volatile("sti");
                     regs->rax = (uint64_t)-1; // poll found nothing
                     return;
                 }
                 Ipc::recv_wait();   // register + SLEEP (IF still off)
                 Sched::yield();     // returns when a send wakes us
             }
        }
        else if (syscall_id == 7) { // OPEN: rdi = path, rsi = flags -> rax = fd
             regs->rax = (uint64_t)VFS::open_fd((const char*)regs->rdi, (int)regs->rsi);
        }
        else if (syscall_id == 8) { // READ: rdi = fd, rsi = buf, rdx = count
             regs->rax = (uint64_t)VFS::read_fd((int)regs->rdi, (void*)regs->rsi, regs->rdx);
        }
        else if (syscall_id == 9) { // WRITE: rdi = fd, rsi = buf, rdx = count
             regs->rax = (uint64_t)VFS::write_fd((int)regs->rdi, (const void*)regs->rsi, regs->rdx);
        }
        else if (syscall_id == 10) { // CLOSE: rdi = fd
             VFS::close_fd((int)regs->rdi);
             regs->rax = 0;
        }
        else if (syscall_id == 6) { // BRK: rdi = new break (or 0 to query).
             uint64_t want = regs->rdi;
             if (Sched::current_task_brk() == 0) {
                 Sched::current_task_set_brk(BRK_BASE);
                 Sched::current_task_set_brk_limit(BRK_BASE);
             }

             if (want == 0) {
                 regs->rax = Sched::current_task_brk();
             } else if (want < BRK_BASE) {
                 regs->rax = (uint64_t)-1;
             } else {
                 // Grow: map [brk_limit, round_up(want)) — brk_limit is the
                 // true mapped top, so nothing gets skipped when brk isn't
                 // page-aligned. (Shrink: accepted, pages stay mapped.)
                 uint64_t limit = Sched::current_task_brk_limit();
                 uint64_t new_top = (want + 0xFFF) & ~0xFFFull;
                 for (uint64_t va = limit; va < new_top; va += 4096) {
                     VMM::map_page(PMM::alloc_page(), va, 0x07); // P|W|U
                 }
                 Sched::current_task_set_brk(want);
                 Sched::current_task_set_brk_limit(new_top);
                 regs->rax = want;
             }
        }
        
        return;
    }

    // --- CPU exceptions (vectors 0..31): the fault contract ---
    // Ring 3 -> kill the offending task, the system carries on.
    // Ring 0 -> full dump, then halt (a kernel bug is not recoverable).
    if (num <= 31) {
        uint64_t cr2 = 0;
        if (num == 14) {
            asm volatile("mov %%cr2, %0" : "=r"(cr2));
        }
        Fault::handle_cpu_exception(regs, cr2); // noreturn
    }

    // --- IRQ 0: Timer (Interrupt 32) ---
    if (num == 32) {
        Timer::tick(); // Update the counter
        Compositor::present(); // repaint pending damage (text/surfaces/cursor)
        PIC::send_eoi(0);

        Sched::yield();

        return;
    }

    // --- IRQ 1: Keyboard ---
    if (num == 33) {
        Keyboard::handle_interrupt(); // Just delegate to the driver
        PIC::send_eoi(1);
        Sched::wake_keyboard_waiter(); // a key may have arrived — wake getc
        return;
    }

    // --- IRQ 12: Mouse (PS/2 aux) ---
    if (num == 44) {
        Mouse::handle_interrupt();
        PIC::send_eoi(12);
        return;
    }

    // --- Unhandled IRQ vectors (34..47 minus the ones handled above) ---
    // EOI them and carry on: a stray event from an unconfigured IRQ line
    // must NOT halt the machine. (We don't kprint — a flapping line would
    // flood the log. Hot-add here when a device needs servicing.)
    if (num >= 32 && num <= 47) {
        PIC::send_eoi(num - 32);
        return;
    }

    kprint("\n!!! UNEXPECTED INTERRUPT !!!\n");
    kprint("Number: ");
    // Print the numeric interrupt vector
    kprint_uint(regs->interrupt_number);
    kprint("\n");
    kprint("Faulting Instruction (RIP): ");
    print_hex64(regs->rip);
    kprint("\n");
    kprint("Halting CPU.\n");
    while(1) asm("hlt");
}
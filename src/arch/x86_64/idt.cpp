#include "idt.h"
#include "pic.h"
#include "kernel/io.h"
#include "kernel/dev/keyboard.h"
#include "kernel/core/fault.h"

#include "kernel/core/timer.h"
#include "kernel/core/sched.h"
#include "kernel/core/ipc.h"
#include "kernel/dev/mouse.h"
#include "kernel/dev/rtc.h"
#include "kernel/gfx/compositor.h"
#include "kernel/gfx/framebuffer.h"
#include "kernel/core/vmm.h"
#include "fs/tar.h"
#include "fs/elf.h"
#include "fs/vfs.h"
#include "kernel/core/pmm.h"
#include "kernel/core/sysinfo.h"
#include "kernel/core/errno.h"
#include "kernel/core/memmap.h"
#include "kernel/core/power.h"
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
                //
                // The one exception is the double fault (vector 8), which gets
                // IST 1 (the stack the GDT already reserves): a #DF usually
                // *means* the current stack is unusable, so pushing its frame
                // there gives a dump full of garbage — RIP=0x8, RSP=0x10 —
                // instead of the state that caused it. Its handler halts and
                // never returns, so the shared-stack hazard above cannot bite.
                uint8_t ist = (i == 8) ? 1 : 0;
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

    // Build the child in its OWN address space. This switches CR3 by hand,
    // so interrupts are turned off and LEFT off: a timer preemption would
    // restore the caller's recorded CR3 and the stack build in
    // create_user_task would fault. The caller re-enables interrupts at the
    // right point — for a foreground spawn, only after the parent has
    // blocked on the child, so the child cannot run first.
    asm volatile("cli");
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

// --- Syscall implementations ---
// One function per syscall number. Each reads its arguments from the trap frame
// (rdi/rsi/rdx) and writes its result to rax; syscall_table maps the number to
// the function, so adding a syscall is one function plus one row here (and the
// matching line in docs/syscalls.md). A nullptr entry is a retired number: the
// dispatcher answers it with -ENOSYS.

using SyscallFn = void (*)(IDT::Registers*);

static void sys_putc(IDT::Registers* regs) {
    // Routed to a bound console owner (a terminal) if there is one, else the
    // kernel console. See Sched::console_emit.
    Sched::console_emit((char)regs->rdi);
}

static void sys_getc(IDT::Registers* regs) {
    // Blocks until a key arrives. Interrupts stay off across the check-and-
    // block so a key landing in between cannot be missed (its IRQ/write wakes
    // us). The owner is re-read every pass: a task bound to a terminal while
    // already blocked switches to that console ring.
    asm volatile("cli");
    int b = -1;
    while (true) {
        if (Sched::console_owner_of_current() != -1) {
            b = Sched::console_ring_getc();
            if (b >= 0) break;
        } else {
            char kc = Keyboard::get_queued_key();
            if (kc != 0) { b = (int)kc; break; }
        }
        Sched::block_on_keyboard();
        Sched::yield();   // returns when a key/write wakes us
    }
    Sched::clear_keyboard_waiting();
    regs->rax = (uint64_t)b;
}

static void sys_listdir(IDT::Registers* regs) {
    VFS::readdir((const char*)regs->rdi, (char*)regs->rsi, regs->rdx);
    regs->rax = 0;
}

static void sys_chdir(IDT::Registers* regs) {
    const char* path = (const char*)regs->rdi;
    if (!VFS::is_directory(path)) {
        regs->rax = (uint64_t)-Errno::ENOENT;
    } else {
        // Store the canonical path (cwd-relative + ./.. resolved).
        static char canon[64];
        VFS::canonicalize(path, canon, sizeof(canon));
        Sched::current_task_set_cwd(canon);
        regs->rax = 0;
    }
}

static void sys_getcwd(IDT::Registers* regs) {
    char* buf = (char*)regs->rdi;
    const char* cwd = Sched::current_task_cwd();
    uint64_t max = regs->rsi;
    uint64_t i = 0;
    while (cwd[i] && i + 1 < max) { buf[i] = cwd[i]; i++; }
    buf[i] = '\0';
    regs->rax = (uint64_t)i;
}

static void sys_sleep(IDT::Registers* regs) {
    // The timer runs at 50Hz (20ms/tick); round up so sleep(1) actually sleeps
    // a tick instead of returning instantly.
    uint64_t ms = regs->rdi;
    uint64_t ticks = (ms + 19) / 20;
    Sched::sleep_current(ticks);
    // Record the sleep intent NOW and switch away at once: without the yield,
    // this task would keep running until a later PIT tick bounced it off.
    asm volatile("cli");
    Sched::yield();
}

static void sys_mouse_read(IDT::Registers* regs) {
    // Non-blocking; rax = 1 if a packet was drained, 0 if none. Once a display
    // authority exists the pointer is its alone (the WM brokers it).
    struct MousePkt { int16_t dx, dy; uint8_t buttons; int8_t dz; };
    MousePkt* out = (MousePkt*)regs->rdi;
    int16_t dx, dy; uint8_t buttons; int8_t dz;
    if (Compositor::has_authority() && !Compositor::is_authority()) {
        regs->rax = 0;
    } else if (Mouse::read_packet(&dx, &dy, &buttons, &dz)) {
        out->dx = dx; out->dy = dy; out->buttons = buttons; out->dz = dz;
        regs->rax = 1;
    } else {
        regs->rax = 0;
    }
}

static void sys_fb_info(IDT::Registers* regs) {
    // Maps the framebuffer into the CALLING task's address space at a fresh
    // user VA; returns the geometry and that address.
    struct FbPkt { uint64_t w, h, pitch, bpp, address, size; };
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

static void sys_ticks(IDT::Registers* regs) {
    regs->rax = Timer::get_ticks();
}

static void sys_surface_create(IDT::Registers* regs) {
    long id = Compositor::surface_create(regs->rdi, regs->rsi);
    if (id >= 0 && regs->rdx) {
        Compositor::Info info;
        if (Compositor::surface_info((int)id, &info))
            *(Compositor::Info*)regs->rdx = info;
    }
    regs->rax = (uint64_t)id;
}

static void sys_surface_commit(IDT::Registers* regs) {
    // rdi = optional {x,y,w,h}* in surface-local pixels (0 = the whole
    // surface), letting a full-screen overlay commit only its change.
    // rsi = the target surface id (0 = all of the caller's).
    struct RectPkt { int64_t x, y, w, h; };
    RectPkt* r = (RectPkt*)regs->rdi;
    int cid = (int)regs->rsi;
    if (r != nullptr)
        Compositor::surface_commit(cid, (int)r->x, (int)r->y, (int)r->w, (int)r->h);
    else
        Compositor::surface_commit(cid, 0, 0, -1, -1);
    regs->rax = 0;
}

static void sys_surface_move(IDT::Registers* regs) {
    Compositor::surface_move((int)regs->rdi, (int)regs->rsi, (int)regs->rdx);
    regs->rax = 0;
}

static void sys_surface_resize(IDT::Registers* regs) {
    long r = Compositor::surface_resize((int)regs->rdi, regs->rsi, regs->rdx);
    if (r == 0 && regs->r10) {
        Compositor::Info info;
        if (Compositor::surface_info((int)regs->rdi, &info))
            *(Compositor::Info*)regs->r10 = info;
    }
    regs->rax = (uint64_t)r;
}

// --- memory mappings (see kernel/core/memmap.h) ---
// A mapping's geometry comes back in the caller's struct, the same shape
// SURFACE_RESIZE uses, so the calls stay three-argument.
struct MapOut { uint64_t address, size; };

static void sys_map_file(IDT::Registers* regs) {
    MapOut* out = (MapOut*)regs->rsi;
    uint64_t addr = 0, size = 0;
    if (MemMap::map_file((const char*)regs->rdi, &addr, &size) != 0) {
        regs->rax = (uint64_t)(long)-1;
        return;
    }
    if (out != nullptr) { out->address = addr; out->size = size; }
    regs->rax = 0;
}

static void sys_munmap(IDT::Registers* regs) {
    regs->rax = (uint64_t)(long)MemMap::unmap(regs->rdi);
}

static void sys_shm_create(IDT::Registers* regs) {
    regs->rax = (uint64_t)(long)MemMap::create(regs->rdi);
}

static void sys_shm_grant(IDT::Registers* regs) {
    regs->rax = (uint64_t)(long)MemMap::grant((int)regs->rdi, (int)regs->rsi);
}

static void sys_shm_map(IDT::Registers* regs) {
    MapOut* out = (MapOut*)regs->rsi;
    uint64_t addr = 0, size = 0;
    if (MemMap::attach((int)regs->rdi, &addr, &size) != 0) {
        regs->rax = (uint64_t)(long)-1;
        return;
    }
    if (out != nullptr) { out->address = addr; out->size = size; }
    regs->rax = 0;
}

static void sys_shm_close(IDT::Registers* regs) {
    regs->rax = (uint64_t)(long)MemMap::close((int)regs->rdi);
}

// Leaving. Neither returns, so nothing is written back to rax.
static void sys_poweroff(IDT::Registers* regs) {
    (void)regs;
    Power::poweroff();
}

static void sys_reboot(IDT::Registers* regs) {
    (void)regs;
    Power::reboot();
}

static void sys_surface_destroy(IDT::Registers* regs) {
    regs->rax = (uint64_t)(long)Compositor::surface_destroy((int)regs->rdi);
}

static void sys_surface_z(IDT::Registers* regs) {
    regs->rax = (uint64_t)(long)Compositor::surface_z((int)regs->rdi, (int64_t)regs->rsi);
}

static void sys_surface_list(IDT::Registers* regs) {
    regs->rax = (uint64_t)(long)Compositor::surface_list(
        (Compositor::Info*)regs->rdi, regs->rsi);
}

static void sys_sysinfo(IDT::Registers* regs) {
    Sysinfo::get((SysInfo*)regs->rdi);
    regs->rax = 0;
}

static void sys_task_list(IDT::Registers* regs) {
    regs->rax = (uint64_t)(long)Sched::task_snapshot(
        (Sched::TaskInfo*)regs->rdi, (int)regs->rsi);
}

static void sys_fault_last(IDT::Registers* regs) {
    regs->rax = Fault::last_fault((Fault::Record*)regs->rdi) ? 1 : 0;
}

static void sys_wm_claim(IDT::Registers* regs) {
    regs->rax = Compositor::claim_authority() ? 0 : (uint64_t)-1;
}

static void sys_input_focus(IDT::Registers* regs) {
    if (Compositor::is_authority()) {
        Sched::set_input_focus((int)regs->rdi);
        regs->rax = 0;
    } else {
        regs->rax = (uint64_t)-1;
    }
}

static void sys_input_mode(IDT::Registers* regs) {
    Sched::set_event_input(regs->rdi != 0);
    regs->rax = 0;
}

static void sys_console_attach(IDT::Registers* regs) {
    regs->rax = Sched::console_attach((int)regs->rdi) ? 0 : (uint64_t)-1;
}

static void sys_console_detach(IDT::Registers* regs) {
    regs->rax = Sched::console_detach((int)regs->rdi) ? 0 : (uint64_t)-1;
}

static void sys_console_read(IDT::Registers* regs) {
    regs->rax = (uint64_t)(long)Sched::console_read((char*)regs->rdi, (int)regs->rsi);
}

static void sys_console_write(IDT::Registers* regs) {
    regs->rax = (uint64_t)(long)Sched::console_write(
        (int)regs->rdi, (const char*)regs->rsi, (int)regs->rdx);
}

static void sys_wm_hotkey(IDT::Registers* regs) {
    // Reserve a chord (e.g. Alt) so those keys reach the authority instead of
    // the focused task — global hotkeys such as Alt+Tab.
    if (Compositor::is_authority()) {
        Compositor::set_hotkey_mask((int)regs->rdi);
        regs->rax = 0;
    } else {
        regs->rax = (uint64_t)-1;
    }
}

static void sys_puts(IDT::Registers* regs) {
    // Stage the string in kernel memory so the whole line is emitted in one
    // atomic kprint (no cross-task interleaving). The cap matches printf's
    // flush chunk (256).
    const char* us = (const char*)regs->rdi;
    char buf[256];
    int i = 0;
    while (us != nullptr && i < 255 && us[i] != '\0') { buf[i] = us[i]; i++; }
    buf[i] = '\0';
    Sched::console_emit_str(buf, i);
    regs->rax = (uint64_t)i;
}

static void sys_lseek(IDT::Registers* regs) {
    regs->rax = (uint64_t)VFS::lseek_fd((int)regs->rdi, (long)regs->rsi, (int)regs->rdx);
}

static void sys_stat(IDT::Registers* regs) {
    struct StatPkt { uint64_t size; uint64_t is_dir; };
    uint64_t sz = 0; bool dir = false;
    int r = VFS::stat_path((const char*)regs->rdi, &sz, &dir);
    if (r == 0 && regs->rsi) {
        StatPkt* out = (StatPkt*)regs->rsi;
        out->size = sz;
        out->is_dir = dir ? 1 : 0;
    }
    regs->rax = (uint64_t)r;
}

static void sys_mkdir(IDT::Registers* regs) {
    regs->rax = (uint64_t)VFS::mkdir((const char*)regs->rdi);
}

static void sys_unlink(IDT::Registers* regs) {
    regs->rax = (uint64_t)VFS::unlink((const char*)regs->rdi);
}

static void sys_rename(IDT::Registers* regs) {
    regs->rax = (uint64_t)VFS::rename((const char*)regs->rdi, (const char*)regs->rsi);
}

static void sys_ftruncate(IDT::Registers* regs) {
    regs->rax = (uint64_t)VFS::ftruncate_fd((int)regs->rdi, (uint64_t)regs->rsi);
}

static void sys_timer_set(IDT::Registers* regs) {
    Sched::timer_arm_current(regs->rdi);
    regs->rax = 0;
}

static void sys_time(IDT::Registers* regs) {
    // Wall-clock seconds since the Unix epoch (anchored by the RTC at boot,
    // advanced by the PIT).
    regs->rax = (uint64_t)Rtc::seconds_now();
}

// SPAWN (4) and SPAWN_BG (21) share this: stage the name, load the ELF, make
// the task. 'background' decides whether we then block on the child.
static void spawn_impl(IDT::Registers* regs, bool background) {
    const char* user_name = (const char*)regs->rdi;
    if (user_name == nullptr) { regs->rax = (uint64_t)-1; return; }

    // Stage the name into kernel memory: the kernel is about to switch CR3, so
    // user pointers may not survive the swap. (Terminate at the true end: the
    // buffer is reused across spawns, and stale chars could leak.)
    int ni = 0;
    while (ni < 63 && user_name[ni] != '\0') {
        spawn_namebuf[ni] = user_name[ni];
        ni++;
    }
    spawn_namebuf[ni] = '\0';

    int child = stage_spawn(regs, spawn_namebuf);   // returns with IRQs off
    if (child < 0) { asm volatile("sti"); regs->rax = (uint64_t)-1; return; }

    if (!background) {
        // Foreground: the child takes over the console the parent is using
        // (the kernel console, or a terminal's hosted one), so it can read and
        // write it. Then block on the child BEFORE it can run — IRQs are still
        // off from stage_spawn, so no timer can switch to the new task until
        // yield(); yield re-enables interrupts once the child's exit resumes us.
        Sched::inherit_console(child);
        Sched::block_current(child);
        Sched::yield();   // returns when the child wakes us
        asm volatile("sti");
    } else {
        asm volatile("sti");
    }
    regs->rax = (uint64_t)child;
}

static void sys_spawn(IDT::Registers* regs)    { spawn_impl(regs, false); }
static void sys_spawn_bg(IDT::Registers* regs) { spawn_impl(regs, true); }

static void sys_exit(IDT::Registers* regs) {
    asm volatile("cli");
    Sched::exit_current(regs->rdi);   // code from RDI — stored for wait
}

static void sys_wait(IDT::Registers* regs) {
    long id = (long)regs->rdi;
    uint64_t code = 0;
    if (id <= 0) {
        regs->rax = (uint64_t)-1;
    } else if (Sched::consume_recent_exit((int)id, &code)) {
        regs->rax = code;
    } else if (Sched::task_exists((int)id)) {
        // Live task: block until it exits (exit fills our inbox).
        asm volatile("cli");
        Sched::block_current((int)id);
        Sched::yield();
        regs->rax = Sched::current_exit_inbox();
    } else {
        regs->rax = (uint64_t)-1;   // unknown task / exit ring overflow
    }
}

static void sys_kill(IDT::Registers* regs) {
    regs->rax = (uint64_t)Sched::kill_task((int)regs->rdi, regs->rsi);
}

static void sys_send(IDT::Registers* regs) {
    // The kernel stamps 'from' with OUR id — senders cannot pose as someone
    // else, and receivers can trust the field.
    const Ipc::Msg* um = (const Ipc::Msg*)regs->rsi;
    if (um == nullptr) { regs->rax = (uint64_t)-1; return; }
    Ipc::Msg m;
    m.from = Sched::current_task_id();
    m.type = um->type;   // user-VA reads run on the caller's CR3
    m.a    = um->a;      // (house pattern: see FBINFO)
    m.b    = um->b;
    regs->rax = (uint64_t)Ipc::send((int)regs->rdi, m);
}

static void sys_recv(IDT::Registers* regs) {
    // The cli-guarded check-and-block loop is the standard discipline (spawn/
    // wait use it too). A send between our check and our waiter registration
    // is impossible with IF off; one after registration wakes us via wake.
    Ipc::Msg* um = (Ipc::Msg*)regs->rdi;
    if (um == nullptr) { regs->rax = (uint64_t)-1; return; }
    while (true) {
        asm volatile("cli");
        Ipc::Msg m;
        if (Ipc::try_recv(&m) == 0) {
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
            regs->rax = (uint64_t)-Errno::EAGAIN;   // a poll found nothing
            return;
        }
        Ipc::recv_wait();   // register + SLEEP (IF still off)
        Sched::yield();     // returns when a send wakes us
    }
}

static void sys_open(IDT::Registers* regs) {
    regs->rax = (uint64_t)VFS::open_fd((const char*)regs->rdi, (int)regs->rsi);
}

static void sys_read(IDT::Registers* regs) {
    regs->rax = (uint64_t)VFS::read_fd((int)regs->rdi, (void*)regs->rsi, regs->rdx);
}

static void sys_write(IDT::Registers* regs) {
    regs->rax = (uint64_t)VFS::write_fd((int)regs->rdi, (const void*)regs->rsi, regs->rdx);
}

static void sys_close(IDT::Registers* regs) {
    VFS::close_fd((int)regs->rdi);
    regs->rax = 0;
}

static void sys_brk(IDT::Registers* regs) {
    uint64_t want = regs->rdi;
    if (Sched::current_task_brk() == 0) {
        Sched::current_task_set_brk(BRK_BASE);
        Sched::current_task_set_brk_limit(BRK_BASE);
    }

    if (want == 0) {
        regs->rax = Sched::current_task_brk();
    } else if (want < BRK_BASE) {
        regs->rax = (uint64_t)-Errno::EINVAL;
    } else {
        // Grow [brk_limit, round_up(want)); brk_limit is the true mapped top,
        // so nothing is skipped when brk is not page-aligned. (Shrink is
        // accepted but pages stay mapped.)
        uint64_t limit = Sched::current_task_brk_limit();
        uint64_t new_top = (want + 0xFFF) & ~0xFFFull;
        for (uint64_t va = limit; va < new_top; va += 4096) {
            VMM::map_page(PMM::alloc_page(), va, 0x07);   // P|W|U
        }
        Sched::current_task_set_brk(want);
        Sched::current_task_set_brk_limit(new_top);
        regs->rax = want;
    }
}

// Number -> handler. A nullptr entry is a retired number.
static const SyscallFn syscall_table[] = {
    /*  0 PUTC            */ sys_putc,
    /*  1 READ (keyboard) */ sys_getc,
    /*  2 retired         */ nullptr,
    /*  3 retired         */ nullptr,
    /*  4 SPAWN           */ sys_spawn,
    /*  5 EXIT            */ sys_exit,
    /*  6 BRK             */ sys_brk,
    /*  7 OPEN            */ sys_open,
    /*  8 READ            */ sys_read,
    /*  9 WRITE           */ sys_write,
    /* 10 CLOSE           */ sys_close,
    /* 11 LISTDIR         */ sys_listdir,
    /* 12 CHDIR           */ sys_chdir,
    /* 13 GETCWD          */ sys_getcwd,
    /* 14 SLEEP           */ sys_sleep,
    /* 15 MOUSE READ      */ sys_mouse_read,
    /* 16 FB INFO+MAP     */ sys_fb_info,
    /* 17 TICKS           */ sys_ticks,
    /* 18 SURFACE CREATE  */ sys_surface_create,
    /* 19 SURFACE COMMIT  */ sys_surface_commit,
    /* 20 SURFACE MOVE    */ sys_surface_move,
    /* 21 SPAWN_BG        */ sys_spawn_bg,
    /* 22 WAIT            */ sys_wait,
    /* 23 KILL            */ sys_kill,
    /* 24 SEND            */ sys_send,
    /* 25 RECV            */ sys_recv,
    /* 26 SURFACE DESTROY */ sys_surface_destroy,
    /* 27 SURFACE Z       */ sys_surface_z,
    /* 28 SURFACE LIST    */ sys_surface_list,
    /* 29 SYSINFO         */ sys_sysinfo,
    /* 30 TASK LIST       */ sys_task_list,
    /* 31 FAULT LAST      */ sys_fault_last,
    /* 32 WM CLAIM        */ sys_wm_claim,
    /* 33 INPUT FOCUS     */ sys_input_focus,
    /* 34 INPUT MODE      */ sys_input_mode,
    /* 35 CONSOLE ATTACH  */ sys_console_attach,
    /* 36 CONSOLE DETACH  */ sys_console_detach,
    /* 37 CONSOLE READ    */ sys_console_read,
    /* 38 CONSOLE WRITE   */ sys_console_write,
    /* 39 WM HOTKEY       */ sys_wm_hotkey,
    /* 40 PUTS            */ sys_puts,
    /* 41 LSEEK           */ sys_lseek,
    /* 42 STAT            */ sys_stat,
    /* 43 MKDIR           */ sys_mkdir,
    /* 44 UNLINK          */ sys_unlink,
    /* 45 FTRUNCATE       */ sys_ftruncate,
    /* 46 TIMER_SET       */ sys_timer_set,
    /* 47 RENAME          */ sys_rename,
    /* 48 TIME            */ sys_time,
    /* 49 SURFACE RESIZE  */ sys_surface_resize,
    /* 50 MAP_FILE        */ sys_map_file,
    /* 51 MUNMAP          */ sys_munmap,
    /* 52 SHM_CREATE      */ sys_shm_create,
    /* 53 SHM_GRANT       */ sys_shm_grant,
    /* 54 SHM_MAP         */ sys_shm_map,
    /* 55 SHM_CLOSE       */ sys_shm_close,
    /* 56 POWEROFF        */ sys_poweroff,
    /* 57 REBOOT          */ sys_reboot,
};

static constexpr uint64_t SYSCALL_COUNT =
    sizeof(syscall_table) / sizeof(syscall_table[0]);

// --- THE C++ HANDLER ---
// This is called by Assembly
extern "C" void interrupt_dispatcher(IDT::Registers* regs) {
    uint64_t num = regs->interrupt_number;

    if (num == 0x80) {
        uint64_t syscall_id = regs->rax;
        if (syscall_id < SYSCALL_COUNT && syscall_table[syscall_id] != nullptr) {
            syscall_table[syscall_id](regs);
        } else {
            // A retired hole or an unknown number is reported, rather than
            // handed back as if the number were a result.
            regs->rax = (uint64_t)-Errno::ENOSYS;
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
        Sched::check_kernel_stack(); // kernel-stack canary (a bug if broken)
        Sched::timer_tick(); // fire any due userland one-shot timers
        Keyboard::tick(); // typematic repeat for a held key
        Compositor::present(); // repaint pending damage (text/surfaces/cursor)
        PIC::send_eoi(0);

        Sched::yield();

        return;
    }

    // --- IRQ 1: Keyboard ---
    if (num == 33) {
        // EOI first: handling a key can interrupt the foreground job, which
        // switches away from this task — and this handler would then never
        // reach a later send_eoi.
        PIC::send_eoi(1);
        Keyboard::handle_interrupt();
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
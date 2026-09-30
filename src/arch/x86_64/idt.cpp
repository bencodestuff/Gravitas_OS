#include "idt.h"
#include "pic.h"
#include "../../kernel/io.h"
#include "../../kernel/keyboard.h"

#include "../../kernel/timer.h"
#include "../../kernel/sched.h"
#include "../../kernel/mouse.h"
#include "../../drivers/framebuffer.h"
#include "../../kernel/vmm.h"
#include "../../fs/tar.h"
#include "../../fs/elf.h"
#include "../../fs/vfs.h"
#include "../../kernel/pmm.h"
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

static char input_buffer[256];
static int buffer_index = 0;

// Link to the assembly table
extern "C" void* isr_stub_table[];
// Explicit syscall stub (vector 128)
extern "C" void isr_128();

static inline bool streq(const char* str1, const char* str2) {
    int i = 0;
    while (str1[i] != '\0' && str2[i] != '\0') {
        if (str1[i] != str2[i]) return false;
        i++;
    }
    return str1[i] == str2[i]; // Check if both ended at the same time
}

namespace IDT {

    // This struct matches exactly what is pushed in Assembly
    struct Registers {
        // Pushed manually in common stub
        uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
        uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
        
        // Pushed by the specific ISR macro
        uint64_t interrupt_number;
        uint64_t error_code;
        
        // Pushed by the CPU automatically
        uint64_t rip;
        uint64_t cs;
        uint64_t rflags;
        uint64_t rsp;
        uint64_t ss;
    } __attribute__((packed));

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

static uint64_t ticks = 0;

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
        else if (syscall_id == 4) { // SPAWN: rdi = name, rsi = argv array, rdx = argc.
             // Blocks until the spawned program exits; rax = child task id.
             asm volatile("cli"); // no timer nesting mid-spawn
             // Copy the name AND argv strings into kernel memory BEFORE the
             // ELF load — loading a program overwrites userland memory
             // (everything shares one address space for now).
             char namebuf[16];
             const char* user_name = (const char*)regs->rdi;
             int ni = 0;
             while (ni < 15 && user_name[ni] != '\0') {
                 namebuf[ni] = user_name[ni];
                 ni++;
             }
             namebuf[ni] = '\0';

             static char argv_stage[8][64];
             const char* argv_copy[8];
             uint64_t user_argc = regs->rdx;
             if (user_argc > 8) user_argc = 8;
             const char** user_argv = (const char**)regs->rsi;
             int argc = 0;
             for (uint64_t i = 0; i < user_argc; i++) {
                 const char* us = user_argv[i];
                 int ci = 0;
                 while (ci < 63 && us[ci] != '\0') {
                     argv_stage[i][ci] = us[ci];
                     ci++;
                 }
                 argv_stage[i][ci] = '\0';
                 argv_copy[i] = argv_stage[i];
                 argc = i + 1;
             }

             // Read the program's ELF image through the VFS (bare names
             // resolve through /bin). Heap buffer, freed after the load.
             int fd = VFS::open_fd(namebuf, VFS::O_RDONLY);
             if (fd < 0) {
                 regs->rax = (uint64_t)-1;
             } else {
                 uint64_t fsize = VFS::fd_size(fd);
                 char* data = new char[fsize];
                 VFS::read_fd(fd, data, fsize);
                 VFS::close_fd(fd);

                 // Build the child in its OWN address space: clone, switch,
                 // load the ELF there, create the task, switch back.
                 uint64_t old_cr3 = VMM::current_pml4();
                 uint64_t child_cr3 = VMM::clone_kernel_space();
                 VMM::switch_pml4(child_cr3);

                 uint64_t entry = ELF::load_elf((uint8_t*)data);
                 delete[] data;
                 if (entry == 0) {
                     VMM::switch_pml4(old_cr3);
                     VMM::destroy_user_space(child_cr3);
                     regs->rax = (uint64_t)-1;
                 } else {
                     int child = Sched::create_user_task(entry, namebuf, argc, argv_copy, child_cr3);
                     VMM::switch_pml4(old_cr3);
                     Sched::block_current(child);
                     Sched::yield(); // returns when the child wakes us
                     regs->rax = (uint64_t)child;
                 }
             }
        }
        else if (syscall_id == 5) { // EXIT: kill the calling task. Never returns.
             asm volatile("cli");
             Sched::exit_current();
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

    if (num == 14) {
        uint64_t cr2;
        asm volatile("mov %%cr2, %0" : "=r"(cr2));

        kprint("\n!!! PAGE FAULT !!!\n");

        kprint("Faulting Address (CR2): ");
        print_hex64(cr2);
        kprint("\n");

        kprint("Faulting Instruction (RIP): ");
        print_hex64(regs->rip);
        kprint("\n");

        kprint("Error Code: ");
        if (regs->error_code & 1) kprint("Present "); else kprint("Not-Present ");
        if (regs->error_code & 2) kprint("Write "); else kprint("Read ");
        if (regs->error_code & 4) kprint("User "); else kprint("Kernel ");
        kprint("\n");
    }

    // --- IRQ 0: Timer (Interrupt 32) ---
    if (num == 32) {
        Timer::tick(); // Update the counter
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

    kprint("\n!!! INTERRUPT RECEIVED !!!\n");
    kprint("Number: "); 
    // Print the numeric interrupt/exception vector
    kprint_uint(regs->interrupt_number);
    kprint("\n");
    kprint("Faulting Instruction (RIP): ");
    print_hex64(regs->rip);
    kprint("\n");

    // Human-friendly names for common exceptions
    if (regs->interrupt_number == 14) {
        kprint("PAGE FAULT (Bad Memory Access)\n");
    } else if (regs->interrupt_number == 13) {
        kprint("GPF (General Protection Fault)\n");
    }

    kprint("Halting CPU.\n");
    while(1) asm("hlt");
}
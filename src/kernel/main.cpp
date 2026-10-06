#include <limine.h>
#include <stddef.h>
#include <stdint.h>

#include "kernel/core/pmm.h"
#include "kernel/core/vmm.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/pic.h"
#include "arch/x86_64/cpu.h"
#include "kernel/io.h"
#include "kernel/core/heap.h"
#include "kernel/core/sched.h"
#include "kernel/klib/klib.h"
#include "kernel/dev/mouse.h"
#include "kernel/core/timer.h"
#include "kernel/gfx/font.h"
#include "kernel/core/fault.h"
#include "kernel/gfx/framebuffer.h"
#include "kernel/gfx/console.h"
#include "kernel/gfx/compositor.h"
#include "fs/elf.h"
#include "fs/tar.h"
#include "fs/vfs.h"
#include "fs/ramfs.h"

#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

// Assembly helper
extern "C" void enter_user_mode(uint64_t rip, uint64_t rsp);

__attribute__((used, section(".requests")))
static volatile LIMINE_BASE_REVISION(3);

__attribute__((used, section(".requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0,
    .response = nullptr,
};

// HHDM request: the kernel-wide window onto all physical memory
__attribute__((used, section(".requests")))
volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0,
    .response = nullptr,
};

__attribute__((used, section(".requests")))
volatile struct limine_kernel_address_request kernel_address_request = {
    .id = LIMINE_KERNEL_ADDRESS_REQUEST,
    .revision = 0,
    .response = nullptr,
};

// The kernel heap lives in the HHDM window at physical 0x08000000 (128MB),
// reserved by the PMM. Every address space shares the HHDM mapping, so the
// heap is visible from the kernel half no matter which process is active —
// no per-space mapping needed and no clone-time surprises.
#define HEAP_PHYS_BASE  0x08000000
#define HEAP_SIZE       0x2000000 // 32MB (holds the compositor's 4MB shadow)

// Boot diagnostics. The scratch self-checks in kmain (PMM page addresses, a
// throwaway VMM mapping, a heap round-trip) are bring-up aids: once the
// machine is known good they are pure noise, so they are compiled but skipped.
// Flip to true when porting to new hardware.
static constexpr bool BOOT_VERBOSE = false;

// Load a program from the tarfs into a fresh address space and start it as
// a ring-3 task. Returns the new task id, or -1 on any failure.
static int spawn_program(const char* path, const char* name) {
    int fd = VFS::open_fd(path, VFS::O_RDONLY);
    if (fd < 0) return -1;

    uint64_t size = VFS::fd_size(fd);
    char* data = new char[size];
    VFS::read_fd(fd, data, size);
    VFS::close_fd(fd);

    // Build the child in its own address space. Interrupts MUST stay off
    // across this: the code switches CR3 by hand (the scheduler does not know
    // about child_cr3 yet), so a timer preemption here would restore the
    // kernel task's recorded CR3 and the stack-build below would fault.
    asm volatile("cli");
    uint64_t old_cr3 = VMM::current_pml4();
    uint64_t child_cr3 = VMM::clone_kernel_space();
    VMM::switch_pml4(child_cr3);
    uint64_t entry = ELF::load_elf((uint8_t*)data);
    delete[] data;

    if (entry == 0) {
        VMM::switch_pml4(old_cr3);
        asm volatile("sti");
        VMM::destroy_user_space(child_cr3);
        return -1;
    }
    int id = Sched::create_user_task(entry, name, 0, nullptr, child_cr3);
    VMM::switch_pml4(old_cr3);
    asm volatile("sti");
    return id;
}

// --- KERNEL MAIN ---
extern "C" void kmain(void) {
    // --- kernel_early: the C++ runtime bootstrap ---
    // 1. Global constructors. Runs before ANY kernel subsystem — the
    //    contract is that ctors are self-contained (no allocation, no
    //    printing, no hardware assumptions; they run in a bare C++
    //    environment and must only touch their own static data).
    //    The array bounds come from linker.ld's .init_array markers.
    int ctor_count = 0;
    {
        extern void (*__init_array_start[])();
        extern void (*__init_array_end[])();
        for (void (**ctor)() = __init_array_start; ctor < __init_array_end; ctor++) {
            (*ctor)();
            ctor_count++;
        }
    }

    // 2. Seed the stack-protector canary before any protected frame runs.
    Fault::init();

    kprint("\n\n=== GRAVITAS OS KERNEL BOOTED ===\n");
    kprint("[init] global ctors run: ");
    kprint_uint(ctor_count);
    kprint("\n");

    // Check Memory Map
    if (memmap_request.response == NULL) {
        kprint("CRITICAL: Limine failed to provide memory map.\n");
        while(1) asm("hlt");
    }

    struct limine_memmap_response *memmap = memmap_request.response;
    uint64_t usable_ram = 0;
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE) {
            usable_ram += entry->length;
        }
    }
    kprint("[mem] usable RAM: ");
    kprint_uint(usable_ram / 1024 / 1024);
    kprint(" MB\n");

    PMM::init(memmap); // pass the memmap struct we got from Limine

    VMM::init(memmap_request.response);
    GDT::init();
    kprint("GDT Initialized.\n");

    // Heap: lives in the HHDM window at a reserved physical range — visible
    // from every address space (the kernel half is shared), no mapping needed
    uint64_t heap_va = hhdm_request.response->offset + HEAP_PHYS_BASE;
    Heap::init((void*)heap_va, HEAP_SIZE);

    // Bring-up self-checks (see BOOT_VERBOSE): two PMM pages must be distinct,
    // a scratch mapping must not fault, and 'new' must hand back usable memory.
    if (BOOT_VERBOSE) {
        uint64_t page_1 = PMM::alloc_page();
        uint64_t page_2 = PMM::alloc_page();
        kprint("[boot] PMM pages: ");
        kprint_uint(page_1); kprint(" and "); kprint_uint(page_2); kprint("\n");
        kprint(page_2 > page_1 ? "[boot] PMM hands out sequential RAM\n"
                               : "[boot] PMM returned out-of-order pages!\n");

        // Flags: Present (1) | Writable (2) = 3
        VMM::map_page(0x1000, 0xDEADBEEF000, 3);
        kprint("[boot] scratch page mapped\n");

        int* probe = new int;
        *probe = 999;
        kprint(*probe == 999 ? "[boot] heap 'new' works\n"
                             : "[boot] heap 'new' FAILED\n");
        delete probe;
    }

    IDT::init();
    kprint("IDT Initialized.\n");

    // x87 + SSE before the scheduler snapshots the FPU template, so every
    // task's fxsave area starts from a clean, SSE-enabled state.
    Cpu::enable_fpu_sse();
    kprint("[CPU] x87 + SSE enabled.\n");

    Sched::init();

    PIC::remap();
    kprint("[PIC] Remapped.\n");
    
    Timer::init();
    PIC::unmask(0); // Enable Timer (IRQ 0)
    PIC::unmask(1); // Enable Keyboard (IRQ 1)
    Mouse::init();
    PIC::unmask(12); // Enable Mouse (IRQ 12)
    kprint("[PIC] IRQ12 mask bit: ");
    kprint_uint(PIC::get_mask(12));
    kprint(" (want 0)\n");

    // --- Filesystem layer ---
    // 1. Locate the tar module (the file tree: /bin, /fonts, /Pictures, /tmp)
    Tar::init();
    // 2. Tar at "/" (read-only, layout = the tar itself); ramfs at /tmp
    //    (writable scratch — the only writable paths in the system).
    VFS::init();
    RamFS::init();
    VFS::mount("/", Tar::vfs_instance());
    VFS::mount("/tmp", RamFS::instance());
    kprint("[VFS] Mounted /(tarfs) + /tmp(ramfs).\n");

    // --- Console font: swap the embedded 8x8 for the PSF2 from the
    // tarfs (never fatal; falls back silently on any failure). On a
    // successful swap, restart the console on the new cell grid —
    // otherwise the 16px glyphs would overwrite the old 8px-spaced
    // boot text mid-screen. (Serial keeps the full early log.)
    if (Font::load_from_disk()) {
        // New metrics: re-derive the cell grid (the old text is discarded,
        // as before) and repaint the freshly empty screen.
        Console::init();
        Compositor::composite();
    }

    // --- VFS self-test: ramfs create/write/read round-trip ---
    {
        int fd = VFS::open_fd("/tmp/selftest.txt", VFS::O_CREATE | VFS::O_WRONLY);
        if (fd < 0) {
            kprint("[VFS] SELF-TEST FAIL: open(O_CREATE)\n");
        } else {
            VFS::write_fd(fd, "ramfs write works", 17);
            VFS::close_fd(fd);

            char rbuf[32] = {0};
            fd = VFS::open_fd("/tmp/selftest.txt", VFS::O_RDONLY);
            VFS::read_fd(fd, rbuf, 31);
            VFS::close_fd(fd);

            const char* expect = "ramfs write works";
            bool ok = true;
            for (int i = 0; i < 17; i++) {
                if (rbuf[i] != expect[i]) { ok = false; break; }
            }
            kprint(ok ? "[VFS] SELF-TEST OK: ramfs write+read round-trip\n"
                      : "[VFS] SELF-TEST FAIL: data mismatch\n");
        }
    }

    // Bring up userland: the console shell, then the window manager. The
    // shell is created first so the WM can find and focus it by name; the
    // WM runs in the background and owns the display (windows, focus,
    // pointer) from boot. If it is missing, a plain console still works.
    if (spawn_program("/bin/shell.elf", "shell") < 0)
        kprint("[Kernel] shell.elf not found or failed to load!\n");

    int wm_id = spawn_program("/bin/wm.elf", "wm.elf");
    if (wm_id < 0) {
        kprint("[Kernel] wm.elf not found — running a plain console.\n");
    } else {
        kprint("[Kernel] window manager as task ");
        kprint_uint((uint64_t)wm_id);
        kprint("\n");
    }

    // The terminal hosts the shell in a window; it needs a WM to place it.
    if (wm_id >= 0) {
        int term_id = spawn_program("/bin/term.elf", "term.elf");
        if (term_id < 0) {
            kprint("[Kernel] term.elf not found — console-only userland.\n");
        } else {
            kprint("[Kernel] terminal as task ");
            kprint_uint((uint64_t)term_id);
            kprint("\n");
        }
    }

    while (1) {
        asm("hlt"); // Idle: the scheduler switches to tasks from timer IRQs
    }
}

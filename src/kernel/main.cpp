#include <limine.h>
#include <stddef.h>
#include <stdint.h>

#include "kernel/core/pmm.h"
#include "kernel/core/vmm.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/pic.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/ioapic.h"
#include "kernel/io.h"
#include "kernel/core/heap.h"
#include "kernel/core/sched.h"
#include "kernel/klib/klib.h"
#include "kernel/dev/mouse.h"
#include "kernel/dev/rtc.h"
#include "kernel/core/timer.h"
#include "kernel/gfx/font.h"
#include "kernel/core/fault.h"
#include "kernel/gfx/framebuffer.h"
#include "kernel/gfx/console.h"
#include "kernel/gfx/compositor.h"
#include "fs/elf.h"
#include "fs/tar.h"
#include "fs/vfs.h"
#include "kernel/core/symbols.h"
#include "kernel/core/env.h"
#include "kernel/core/block.h"
#include "kernel/dev/memblock.h"
#include "kernel/dev/modules.h"
#include "kernel/dev/ata.h"
#include "kernel/dev/virtio.h"
#include "kernel/dev/ext2.h"
#include "fs/ramfs.h"
#include "fs/overlay.h"

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
    uint64_t entry = ELF::load_elf((uint8_t*)data, size);
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
    kprint_lus("[init] global ctors run: ", (uint64_t)ctor_count, "\n");

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
    kprint_lus("[mem] usable RAM: ", usable_ram / 1024 / 1024, " MB\n");

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

    // --- Interrupt controllers: take them over, then switch interrupts on ---
    //
    // All of this happens with interrupts still off, which is the point. The PIC
    // is reprogrammed here, and reprogramming it with interrupts live is how the
    // machine used to die: an interrupt would be delivered *during* the ICW
    // sequence, while the vector mapping still meant something else. The fault
    // it produced looked like a double fault with a nonsense frame, and the
    // address in that frame was this function.
    //
    // Order matters: remap so IRQ0-7 land on 32-39 (not 8-15, which are
    // exceptions), then silence every controller, then enable interrupts. The
    // kernel unmasks exactly what it wants, below.
    PIC::remap();
    PIC::mask_all();
    Cpu::disable_local_apic();   // interrupts go through the PIC here
    Ioapic::mask_all();          // ...and never through the IOAPIC
    asm volatile("sti");
    kprint("[PIC] Remapped; interrupts on.\n");

    Timer::init();
    Rtc::init();   // anchor the wall clock before anything reads it
    PIC::unmask(0); // Enable Timer (IRQ 0)
    PIC::unmask(1); // Enable Keyboard (IRQ 1)
    Mouse::init();
    PIC::unmask(12); // Enable Mouse (IRQ 12)
    kprint("[PIC] IRQ12 mask bit: ");
    kprint_uint(PIC::get_mask(12));
    kprint(" (want 0)\n");

    // The Block layer's boot self-test needs a device that is nobody's data, so
    // it runs on a small RAM disk of its own. 64 sectors: the test needs eight,
    // and the real data disk comes from the machine or the bootloader.
    Block::Device* scratch = MemBlock::create("ram0", 64, nullptr, 0);
    if (scratch == nullptr) {
        kprint("[Block] no scratch device\n");
    } else {
        kprint(Block::self_test(scratch) ? "[Block] self-test ok\n"
                                        : "[Block] self-test FAILED\n");
    }

    // --- Filesystem layer ---
    // 1. Locate the tar module (the file tree: /bin, /fonts, /Pictures, /tmp)
    Tar::init();
    // 2. The writable root: the tar image is the read-only lower layer and a
    //    ramfs is the upper layer over it, so "/" is writable — writes land in
    //    the upper and never touch the image. /tmp is just a directory in it.
    VFS::init();
    RamFS::init();
    Overlay::init(RamFS::instance(), Tar::vfs_instance());
    VFS::mount("/", Overlay::instance());
    kprint("[VFS] Mounted / (overlay: ramfs over tarfs).\n");

    // 3. The data disk: a virtio-blk device when the machine has one, else a
    //    real ATA drive, else the disk.img module copied into a RAM disk.
    //    Either way ext2 goes on top of it, mounted read-write, and the rest of
    //    the system cannot tell which. The RAM-disk copy is writable too: the
    //    image keeps the guest's changes until the machine is gone, and the
    //    image on the host is never touched.
    Block::Device* disk = Virtio::init();  // logs [Virtio] ... when it finds one
    if (disk == nullptr) disk = Ata::init();   // logs [ATA] ... likewise
    if (disk == nullptr) {
        Modules::Module img;
        if (Modules::find(".img", &img) && img.size > 0) {
            uint64_t sectors = (img.size + Block::SECTOR - 1) / Block::SECTOR;
            disk = MemBlock::create("disk0", sectors, img.data, img.size);
            if (disk != nullptr)
                kprint("[Disk] no virtio or ATA drive; disk.img copied into RAM\n");
        }
    }

    // A drive the controller has besides the data disk is not used as data by
    // this kernel, so it is scratch: the test machine attaches one (see
    // tests/harness.py) and the write path gets exercised without anything
    // being written to the disk the filesystem lives on.
    for (int i = 1; i < Virtio::count(); i++) {
        Block::Device* scratch = Virtio::device(i);
        kprint(Block::self_test(scratch)
                   ? "[Block] scratch drive self-test ok\n"
                   : "[Block] scratch drive self-test FAILED\n");
    }
    for (int i = 1; i < Ata::count(); i++) {
        Block::Device* scratch = Ata::device(i);
        kprint(Block::self_test(scratch)
                   ? "[Block] scratch drive self-test ok\n"
                   : "[Block] scratch drive self-test FAILED\n");
    }

    if (disk == nullptr) {
        kprint("[Disk] no disk image\n");
    } else {
        VFS::Filesystem* ext2 = Ext2::mount(disk, true);
        if (ext2 == nullptr) {
            kprint("[Ext2] the disk is not ext2\n");
        } else if (!VFS::mount("/disk", ext2)) {
            kprint("[Ext2] no room in the mount table\n");
        }
    }

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

    // The symbol table for the fault handler. It is generated at build time and
    // read here rather than linked in, because a table of the kernel's symbols
    // changes the addresses it would be describing. A missing table just means
    // faults print bare addresses.
    {
        static char symtab[256 * 1024];
        uint64_t symsize = 0;
        if (VFS::read_full("/symbols.bin", symtab, sizeof(symtab), &symsize) &&
            symsize > 0) {
            Symbols::init(symtab, symsize);
            kprint_lus("[Symbols] functions named: ", (uint64_t)Symbols::count(), "\n");
            // The table covers the kernel and every program. A userland fault is
            // easy to see in a test; this proves our own half resolves too.
            char self[80];
            if (Symbols::resolve((uint64_t)&kprint, self, sizeof(self)))
                kprint3("[Symbols] self-check: ", self, "\n");
        } else {
            kprint("[Symbols] no table (faults will print bare addresses)\n");
        }
    }

    // The environment every program starts with: a file, read once, copied onto
    // each new task's stack (see kernel/core/env.h).
    {
        static char envfile[1024];
        uint64_t envlen = 0;
        if (VFS::read_full("/env", envfile, sizeof(envfile), &envlen) && envlen > 0) {
            Env::set(envfile, envlen);
            kprint_lus("[Env] variables: ", (uint64_t)Env::count(), "\n");
        }
    }

    // Bring up userland. The kernel starts exactly ONE program — init — and
    // then idles; init owns boot policy from there (the manifest at
    // /init.lua decides which programs start, in what order, and what to do if
    // one fails). That is the same rule the rest of the tree follows: the
    // kernel owns mechanisms, userland owns policy. Before this, the spawn
    // order of the shell, WM and terminal was hardcoded right here.
    if (spawn_program("/bin/init.elf", "init") < 0) {
        // No init means nothing runs the manifest. A console shell is the one
        // fallback that keeps the machine usable, so take it rather than dying.
        kprint("[Kernel] init.elf not found; falling back to a console shell.\n");
        if (spawn_program("/bin/shell.elf", "shell") < 0)
            kprint("[Kernel] shell.elf not found either — console only.\n");
    }

    while (1) {
        asm("hlt"); // Idle: the scheduler switches to tasks from timer IRQs
    }
}

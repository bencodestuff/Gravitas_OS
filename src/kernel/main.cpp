#include <limine.h>
#include <stddef.h>
#include <stdint.h>

#include "pmm.h"
#include "vmm.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/pic.h"
#include "io.h"
#include "heap.h"
#include "shell.h"
#include "sched.h"
#include "timer.h"
#include "../drivers/framebuffer.h"
#include "../fs/elf.h"
#include "../fs/tar.h"

#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

// Assembly helper
extern "C" void enter_user_mode(uint64_t rip, uint64_t rsp);

__attribute__((used, section(".requests")))
static volatile LIMINE_BASE_REVISION(3);

__attribute__((used, section(".requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0
};

// MOVED FROM PMM/VMM: The HHDM Request
__attribute__((used, section(".requests")))
volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0
};

__attribute__((used, section(".requests")))
volatile struct limine_kernel_address_request kernel_address_request = {
    .id = LIMINE_KERNEL_ADDRESS_REQUEST,
    .revision = 0
};

// Declared in initrd.cpp
extern volatile struct limine_module_request module_request;

// The kernel heap lives in the HHDM window at physical 0x08000000 (128MB),
// reserved by the PMM. Every address space shares the HHDM mapping, so the
// heap is visible from the kernel half no matter which process is active —
// no per-space mapping needed and no clone-time surprises.
#define HEAP_PHYS_BASE  0x08000000
#define HEAP_SIZE       0x400000 // 4MB

// --- Helper: Print Numbers to Serial ---
extern "C" void kprint_uint(uint64_t value) {
    if (value == 0) {
        kprint("0");
        return;
    }
    char buffer[21];
    int pos = 20;
    buffer[pos] = '\0';
    while (value > 0) {
        buffer[--pos] = (value % 10) + '0';
        value /= 10;
    }
    kprint(&buffer[pos]);
}

// User space address definitions
#define USER_CODE_ADDR 0x400000
#define USER_STACK_ADDR 0x500000

void load_and_run_user_program() {
    // 1. Find the userland shell in the tarfs
    char* data = nullptr;
    uint64_t size = 0;
    if (!Tar::read_file("shell.elf", &data, &size)) {
        kprint("No shell.elf in tarfs. Cannot run user program.\n");
        return;
    }

    kprint("[Kernel] Loading shell.elf from tarfs...\n");

    // 2. Load the ELF (Allocates RAM and copies code)
    uint64_t entry_point = ELF::load_elf((uint8_t*)data);

    if (entry_point == 0) {
        kprint("[Kernel] ELF Load Failed.\n");
        return;
    }

    kprint("[Kernel] ELF Loaded. Jumping to User Mode...\n");

    // 3. Setup Stack
    // We reuse the same stack location as our manual test: 0x500000
    // But we need to make sure it's mapped! 
    // (The ELF loader mapped code, but usually not the stack).
    uint64_t stack_phys = (uint64_t)PMM::alloc_page();
    VMM::map_page(stack_phys, USER_STACK_ADDR, 0x07);
    
    // 4. Flush TLB (Important after all that mapping)
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    asm volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");

    // 5. Jump!
    // We jump to 'entry_point' (from the ELF header), not a hardcoded address.
    
    // Set Safety Net
    uint64_t kernel_stack;
    asm volatile("mov %%rsp, %0" : "=r"(kernel_stack));
    GDT::tss.rsp0 = kernel_stack;

    enter_user_mode(entry_point, USER_STACK_ADDR + 4096);
}

// A simple function for our second thread
// ... includes ...



void jump_to_user_mode_test() {
    kprint("[User] Preparing Ring 3 Jump...\n");

    // 1. Allocation
    uint64_t phys_code = (uint64_t)PMM::alloc_page();
    if (phys_code == 0) { kprint("PMM returned 0! Fix PMM loop.\n"); return; }
    
    // 2. Map (This updates the Page Tables in RAM)
    VMM::map_page(phys_code, USER_CODE_ADDR, 0x07); 
    uint64_t phys_stack = (uint64_t)PMM::alloc_page();
    VMM::map_page(phys_stack, USER_STACK_ADDR, 0x07);

    // --- THE FIX: NUCLEAR TLB FLUSH ---
    // invlpg is not enough because we modified the Upper Level Tables (PML4) permissions.
    // We must reload CR3 to flush the "Paging Structure Caches".
    uint64_t cr3_val;
    asm volatile("mov %%cr3, %0" : "=r"(cr3_val));
    asm volatile("mov %0, %%cr3" :: "r"(cr3_val) : "memory");
    // ----------------------------------

    // 3. Write the Program (NOW the CPU is guaranteed to see the right page)
    // Prefer writing via HHDM (physical + offset) to avoid any virtual mapping races.
        uint64_t hhdm_off = 0;
        if (hhdm_request.response != nullptr) hhdm_off = hhdm_request.response->offset;
        uint8_t* pphys = (uint8_t*)(uintptr_t)(phys_code + hhdm_off);
        uint8_t* code = (uint8_t*)USER_CODE_ADDR; // virtual view for sanity checks

        int i = 0;
        // mov rax, 0
        pphys[i++] = 0x48; pphys[i++] = 0xC7; pphys[i++] = 0xC0;
        pphys[i++] = 0x00; pphys[i++] = 0x00; pphys[i++] = 0x00; pphys[i++] = 0x00;

        // mov rdi, 'A'
        pphys[i++] = 0x48; pphys[i++] = 0xC7; pphys[i++] = 0xC7;
        pphys[i++] = 0x41; pphys[i++] = 0x00; pphys[i++] = 0x00; pphys[i++] = 0x00;

        // int 0x80
        pphys[i++] = 0xCD; pphys[i++] = 0x80;

        // jmp $ (place after syscall so it doesn't overwrite it)
        pphys[16] = 0xEB; pphys[17] = 0xFE;

        // Make sure stores complete before we probe
        asm volatile ("" ::: "memory");

    // 4. Verify Readback (Sanity Check) - first opcode should be 0x48 (mov rax,imm)
    if (code[0] != 0x48) {
        kprint("ERROR: Memory Write Failed even after Flush!\n");
        kprint("[User] Debug: code[0] = "); kprint_uint((uint64_t)code[0]); kprint("\n");
        return;
    }

    // Debug: check mapping and readable content
    kprint("[User] Debug: code mapped? "); kprint(VMM::is_mapped(USER_CODE_ADDR) ? "YES\n" : "NO\n");
    kprint("[User] Debug: stack mapped? "); kprint(VMM::is_mapped(USER_STACK_ADDR) ? "YES\n" : "NO\n");
    kprint("[User] Debug: HHDM pphys[0] = "); kprint_uint((uint64_t)pphys[0]); kprint("\n");
    kprint("[User] Debug: VIRT  code[0] = "); kprint_uint((uint64_t)code[0]); kprint("\n");

    // 5. Set Safety Net
    uint64_t kernel_stack;
    asm volatile("mov %%rsp, %0" : "=r"(kernel_stack));
    GDT::tss.rsp0 = kernel_stack;

    kprint("[User] Jumping...\n");
    enter_user_mode(USER_CODE_ADDR, USER_STACK_ADDR + 4096);
}

// --- KERNEL MAIN ---
extern "C" void kmain(void) {
    kprint("\n\n=== GRAVITAS OS KERNEL BOOTED ===\n");
    kprint("If you can read this, the kernel is alive!\n");

    // Check Memory Map
    if (memmap_request.response == NULL) {
        kprint("CRITICAL: Limine failed to provide memory map.\n");
        while(1) asm("hlt");
    }

    struct limine_memmap_response *memmap = memmap_request.response;
    uint64_t usable_ram = 0;

    kprint("Scanning Memory Map...\n");

    // Loop through memory
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];

        if (entry->type == LIMINE_MEMMAP_USABLE) {
            usable_ram += entry->length;
        }
    }

    // Print the findings
    kprint("Total Usable RAM: ");
    kprint_uint(usable_ram);
    kprint(" bytes (");
    kprint_uint(usable_ram / 1024 / 1024);
    kprint(" MB)\n");
    kprint("=== END OF REPORT ===\n");

    kprint("Initializing PMM...\n");
    PMM::init(memmap); // pass the memmap struct we got from Limine
    kprint("PMM Initialized.\n");

// --- TEST ALLOCATION ---
    kprint("\nTesting Allocation...\n");
    
    // 1. Ask for a page
    uint64_t page_1 = PMM::alloc_page();
    kprint("Page 1 allocated at: ");
    kprint_uint(page_1);           // Print to serial
    kprint("\n");

    // 2. Ask for another page
    uint64_t page_2 = PMM::alloc_page();
    kprint("Page 2 allocated at: ");
    kprint_uint(page_2);
    kprint("\n");

    // 3. Verify they are different
    if (page_2 > page_1) {
        kprint("SUCCESS: Allocator is handing out sequential RAM!\n");
    } else {
        kprint("ERROR: Allocator returned weird addresses.\n");
    }
    VMM::init(memmap_request.response);    // TEST: Try to map a fake address
    // We will map Physical 0x1000 to Virtual 0xDEADBEEF000
    kprint("Mapping test page...\n");
    
    GDT::init();
    kprint("GDT Initialized.\n");
    // Flags: Present (1) | Writable (2) = 3
    VMM::map_page(0x1000, 0xDEADBEEF000, 3);
    
    kprint("Page mapped successfully (No Crash).\n");

    // Heap: lives in the HHDM window at a reserved physical range — visible
    // from every address space (the kernel half is shared), no mapping needed
    uint64_t heap_va = hhdm_request.response->offset + HEAP_PHYS_BASE;
    Heap::init((void*)heap_va, HEAP_SIZE);

    // TEST: Dynamic Allocation
    int* test_ptr = new int;
    *test_ptr = 999;
    kprint("Heap Test: ");
    if (*test_ptr == 999) {
        kprint("SUCCESS! 'new' operator works.\n");
    } else {
        kprint("FAILED.\n");
    }

    IDT::init();
    kprint("IDT Initialized.\n");

    Sched::init();

    PIC::remap();
    kprint("[PIC] Remapped.\n");
    
    Timer::init();
    PIC::unmask(0); // Enable Timer (IRQ 0)
    PIC::unmask(1); // Enable Keyboard (IRQ 1)

    Tar::init();

    // Spawn the userland shell as a real ring-3 task in its own address space
    char* shell_data = nullptr;
    uint64_t shell_size = 0;
    if (Tar::read_file("shell.elf", &shell_data, &shell_size)) {
        uint64_t old_cr3 = VMM::current_pml4();
        uint64_t child_cr3 = VMM::clone_kernel_space();
        VMM::switch_pml4(child_cr3);

        uint64_t entry = ELF::load_elf((uint8_t*)shell_data);
        if (entry != 0) {
            Sched::create_user_task(entry, "shell", 0, nullptr, child_cr3);
            VMM::switch_pml4(old_cr3);
        } else {
            VMM::switch_pml4(old_cr3);
            VMM::destroy_user_space(child_cr3);
            kprint("[Kernel] shell.elf failed to load!\n");
        }
    } else {
        kprint("[Kernel] No shell.elf in tarfs!\n");
    }

    while (1) {
        asm("hlt"); // Idle: the scheduler switches to tasks from timer IRQs
    }
}

#include <limine.h>
#include <stddef.h>
#include <stdint.h>

#include "pmm.h"
#include "vmm.h"
#include "gdt.h"
#include "idt.h"
#include "pic.h"
#include "io.h"
#include "heap.h"
#include "shell.h"
#include "sched.h"
#include "timer.h"
#include "../drivers/framebuffer.h"

#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

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



// Arbitrary start address for our heap (somewhere far away from the kernel)
// 0x1000 0000 0000 fits nicely in 64-bit space.
#define HEAP_START_ADDR 0x100000000000
#define HEAP_INITIAL_PAGES 256 // 256 * 4KB = 1 MB of Heap to start

void heap_init_pages() {
    kprint("[Heap] Mapping pages...\n");
    
    // We need to verify we have enough RAM for this, but for now let's assume we do.
    // We are going to ask the PMM for pages and map them to our Heap Address.
    
    uint64_t virt_addr = HEAP_START_ADDR;
    
    for (int i = 0; i < HEAP_INITIAL_PAGES; i++) {
        // 1. Allocate a physical page
        uint64_t phys_addr = (uint64_t)PMM::alloc_page();
        if (phys_addr == 0) {
            kprint("[Heap] OUT OF RAM!\n");
            return;
        }

        // 2. Map it to the virtual address
        // Flags: Present | Writable (No User access yet)
        // Note: We use the defaults, caching is fine here.
        VMM::map_page(phys_addr, virt_addr, 0x03); 
        
        virt_addr += 4096;
    }
    kprint("[Heap] Pages mapped.\n");
}

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

// A simple function for our second thread
void background_worker() {
    while (true) {
        kprint(" [BG] ");

        // Waste some time so we don't spam
        Sched::sleep(500);
        // We must manually yield for now (Cooperative Multithreading)
        // because we haven't hooked the Timer Interrupt to force it yet.
        Sched::yield(); 
    }
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

    heap_init_pages();
    Heap::init((void*)HEAP_START_ADDR, HEAP_INITIAL_PAGES * 4096);

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

    PIC::remap();
    PIC::unmask(0); // Enable Timer (IRQ 0)
    PIC::unmask(1); // Enable Keyboard (IRQ 1)
    
    Shell::init();

    Timer::init();
    Sched::init();
    Sched::create_task(background_worker);
    // The Infinite Kernel Loop
    while (1) {
        // 1. Process Shell Input
        Shell::update();
        Sched::yield(); // Give other tasks a chance to run
        // 2. Halt CPU until next interrupt (saves power)
        // If we don't halt, the CPU spins at 100% heat checking the buffer.
        //asm("hlt");
    }
}

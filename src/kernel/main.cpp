#include <limine.h>
#include <stddef.h>
#include <stdint.h>
#include "pmm.h"

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

extern "C" void serial_print(const char* str) {
    for (int i = 0; str[i] != '\0'; i++) {
        outb(0x3f8, str[i]); // 0x3f8 is the address of COM1
    }
}

#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

__attribute__((used, section(".requests")))
static volatile LIMINE_BASE_REVISION(3);

__attribute__((used, section(".requests")))
static volatile struct limine_terminal_request terminal_request = {
    .id = LIMINE_TERMINAL_REQUEST,
    .revision = 0
};

__attribute__((used, section(".requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0
};

// --- Helper: Print Numbers to Serial ---
void serial_print_uint(uint64_t value) {
    if (value == 0) {
        serial_print("0");
        return;
    }
    char buffer[21];
    int pos = 20;
    buffer[pos] = '\0';
    while (value > 0) {
        buffer[--pos] = (value % 10) + '0';
        value /= 10;
    }
    serial_print(&buffer[pos]);
}

// --- KERNEL MAIN ---
extern "C" void kmain(void) {
    serial_print("\n\n=== GRAVITAS OS KERNEL BOOTED ===\n");
    serial_print("If you can read this, the kernel is alive!\n");

    // Check Memory Map
    if (memmap_request.response == NULL) {
        serial_print("CRITICAL: Limine failed to provide memory map.\n");
        while(1) asm("hlt");
    }

    struct limine_memmap_response *memmap = memmap_request.response;
    uint64_t usable_ram = 0;

    serial_print("Scanning Memory Map...\n");

    // Loop through memory
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];

        if (entry->type == LIMINE_MEMMAP_USABLE) {
            usable_ram += entry->length;
        }
    }

    // Print the findings
    serial_print("Total Usable RAM: ");
    serial_print_uint(usable_ram);
    serial_print(" bytes (");
    serial_print_uint(usable_ram / 1024 / 1024);
    serial_print(" MB)\n");
    serial_print("=== END OF REPORT ===\n");

    serial_print("Initializing PMM...\n");
    PMM::init(memmap); // pass the memmap struct we got from Limine
    serial_print("PMM Initialized.\n");

// --- TEST ALLOCATION ---
    serial_print("\nTesting Allocation...\n");
    
    // 1. Ask for a page
    uint64_t page_1 = PMM::alloc_page();
    serial_print("Page 1 allocated at: ");
    // print_uint(term, term_resp, page_1); // Print to screen (optional)
    serial_print_uint(page_1);           // Print to serial
    serial_print("\n");

    // 2. Ask for another page
    uint64_t page_2 = PMM::alloc_page();
    serial_print("Page 2 allocated at: ");
    serial_print_uint(page_2);
    serial_print("\n");

    // 3. Verify they are different
    if (page_2 > page_1) {
        serial_print("SUCCESS: Allocator is handing out sequential RAM!\n");
    } else {
        serial_print("ERROR: Allocator returned weird addresses.\n");
    }

    // Halt the CPU
    while (1) {
        asm("hlt");
    }
}

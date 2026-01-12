#include <limine.h>
#include <stddef.h>
#include <stdint.h>

#include "pmm.h"
#include "vmm.h"
#include "gdt.h"
#include "idt.h"
#include "pic.h"
#include "io.h"
#include "font.h"


// REPLACE your old kprint with this:
// Forward declare screen_print so `kprint` can call it.
void screen_print(const char* str);


extern "C" void kprint(const char* str) {
    // 1. Print to Host Terminal (Serial)
    // We recreate the serial logic here briefly or call a helper
    for (int i = 0; str[i] != '\0'; i++) {
        outb(0x3f8, str[i]); 
    }

    // 2. Print to QEMU Window (Screen)
    screen_print(str);
}

#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

__attribute__((used, section(".requests")))
static volatile LIMINE_BASE_REVISION(3);

__attribute__((used, section(".requests")))
volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0
};

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

// Helper: Put a pixel on the screen
void put_pixel(int x, int y, uint32_t color) {
    if (framebuffer_request.response == nullptr || framebuffer_request.response->framebuffer_count < 1) return;
    struct limine_framebuffer* fb = framebuffer_request.response->framebuffers[0];

    // Safety check
    if (x < 0 || x >= (int)fb->width || y < 0 || y >= (int)fb->height) return;

    // Calculate memory location: Base + (y * pitch) + (x * bytes_per_pixel)
    uint8_t* base = (uint8_t*) (uintptr_t) fb->address;
    uint64_t offset = (uint64_t)y * fb->pitch + (uint64_t)x * 4;
    uint32_t* ptr = (uint32_t*)(base + offset);
    *ptr = color;
}

// Cursor position
static int cursor_x = 0;
static int cursor_y = 0;

void screen_print(const char* str) {
    // 1. Safety check
    if (framebuffer_request.response == nullptr) return;
    struct limine_framebuffer* fb = framebuffer_request.response->framebuffers[0];

    // Colors
    uint32_t color_text = 0x00FFFFFF; // White
    uint32_t color_bg   = 0x00000000; // Black

    for (int i = 0; str[i] != '\0'; i++) {
        char c = str[i];

        if (c == '\b') {
            if (cursor_x >= 8) {
                cursor_x -= 8; // Move cursor back
                // Manually erase the character by drawing a black square
                for (int y = 0; y < 8; y++) {
                    for (int x = 0; x < 8; x++) {
                        put_pixel(cursor_x + x, cursor_y + y, color_bg);
                    }
                }
            }
            continue; // Move to next character in string
        }

        // --- Handle Newline ---
        if (c == '\n') {
            cursor_x = 0;
            cursor_y += 8;
            continue;
        }

        // --- Draw Printable Characters ---
        if (c >= 32 && c <= 126) {
            int index = c - 32; 

            for (int y = 0; y < 8; y++) {
                uint8_t row = font8x8_basic[index][y];
                for (int x = 0; x < 8; x++) {
                    // If bit is 1, draw White. If 0, draw Black.
                    if ((row >> (7 - x)) & 1) {
                        put_pixel(cursor_x + x, cursor_y + y, color_text);
                    } else {
                        put_pixel(cursor_x + x, cursor_y + y, color_bg);
                    }
                }
            }
            cursor_x += 8; 

            // Simple line wrap if we hit the right edge
            if (cursor_x >= (int)fb->width) {
                cursor_x = 0;
                cursor_y += 8;
            }
        }
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

    IDT::init();
    kprint("IDT Initialized.\n");

    PIC::remap();
    PIC::unmask(1); // Enable Keyboard (IRQ 1)
    
    kprint("Keyboard Enabled. Press keys in the QEMU window!\n");

    // Halt the CPU
    while (1) {
        asm("hlt");
    }
}

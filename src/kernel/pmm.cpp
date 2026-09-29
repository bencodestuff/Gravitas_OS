#include "pmm.h"

// Allow this file to use the serial printer from main.cpp
extern "C" void kprint(const char* str);

extern volatile struct limine_hhdm_request hhdm_request;

// Debug printer
static void pmm_print_hex(uint64_t value) {
    char buffer[20];
    int pos = 0; 
    buffer[pos++] = '0'; buffer[pos++] = 'x';
    if (value == 0) {
        buffer[pos++] = '0';
    } else {
        for (int i = 60; i >= 0; i -= 4) {
            uint8_t nibble = (value >> i) & 0xF;
            if (nibble == 0 && pos == 2) continue;
            buffer[pos++] = (nibble < 10) ? (nibble + '0') : (nibble - 10 + 'A');
        }
    }
    buffer[pos++] = '\n';
    buffer[pos] = '\0';
    kprint(buffer);
}

namespace PMM {
    static uint8_t* bitmap = nullptr;
    static uint64_t bitmap_size = 0;
    static uint64_t highest_addr = 0;

    static void memory_set(void* ptr, int value, uint64_t num) {
        uint8_t* u8ptr = (uint8_t*)ptr;
        for (uint64_t i = 0; i < num; i++) u8ptr[i] = (uint8_t)value;
    }

    static void bitmap_set(uint64_t bit_index) {
        bitmap[bit_index / 8] |= (1 << (bit_index % 8));
    }

    static void bitmap_unset(uint64_t bit_index) {
        bitmap[bit_index / 8] &= ~(1 << (bit_index % 8));
    }

    static bool bitmap_test(uint64_t bit_index) {
        return (bitmap[bit_index / 8] & (1 << (bit_index % 8))) != 0;
    }

    void init(struct limine_memmap_response* map) {
        kprint("[PMM] Scanning RAM...\n");
        // Get the HHDM Offset
        if (hhdm_request.response == NULL) {
            kprint("CRITICAL: HHDM Request failed!\n");
            for(;;) asm("hlt");
        }
        uint64_t hhdm_offset = hhdm_request.response->offset;
        
        kprint("[PMM] HHDM Offset: "); pmm_print_hex(hhdm_offset);

        // Find range
        highest_addr = 0;
        for (uint64_t i = 0; i < map->entry_count; i++) {
            struct limine_memmap_entry* entry = map->entries[i];
            uint64_t top = entry->base + entry->length;
            if (entry->type == LIMINE_MEMMAP_USABLE && top > highest_addr) {
                highest_addr = top;
            }
        }
        
        uint64_t total_pages = highest_addr / PAGE_SIZE;
        bitmap_size = total_pages / 8;
        
        // Find space for the bitmap
        for (uint64_t i = 0; i < map->entry_count; i++) {
            struct limine_memmap_entry* entry = map->entries[i];
            if (entry->type == LIMINE_MEMMAP_USABLE && entry->length >= bitmap_size) {
                
                bitmap = (uint8_t*)(entry->base + hhdm_offset);
                
                break;
            }
        }

        if (bitmap == nullptr) {
            kprint("CRITICAL: No RAM for bitmap!\n");
            for(;;) asm("hlt");
        }

        kprint("[PMM] Bitmap VIRTUAL Address: "); pmm_print_hex((uint64_t)bitmap);

        // Initialize (Mark all used) - This triggered the crash before!
        memory_set(bitmap, 0xFF, bitmap_size); 

        // Unlock usable regions
        for (uint64_t i = 0; i < map->entry_count; i++) {
             struct limine_memmap_entry* entry = map->entries[i];
             if (entry->type == LIMINE_MEMMAP_USABLE) {
                 for (uint64_t j = 0; j < entry->length; j += PAGE_SIZE) {
                     bitmap_unset((entry->base + j) / PAGE_SIZE);
                 }
             }
        }

        // Lock Bitmap itself
        // Note: bitmap pointer is now Virtual, need the Physical address to calculate the index
        uint64_t bitmap_phys_addr = (uint64_t)bitmap - hhdm_offset;
        uint64_t bitmap_start_page = bitmap_phys_addr / PAGE_SIZE;
        uint64_t bitmap_page_count = (bitmap_size + PAGE_SIZE - 1) / PAGE_SIZE;
        
        for (uint64_t i = 0; i < bitmap_page_count; i++) {
            bitmap_set(bitmap_start_page + i);
        }

        // Lock first 1MB
        for (uint64_t i = 0; i < 256; i++) bitmap_set(i);

        // Lock the kernel heap's physical range so alloc_page() never hands
        // those frames to anyone else. The heap lives at HHDM+0x08000000,
        // which is visible in every address space (kernel half is shared).
        reserve_range(0x08000000, 0x400000); // 4MB for the kernel heap

        kprint("[PMM] Initialized successfully.\n");
    }

    // Mark a physical range as permanently in use
    void reserve_range(uint64_t base, uint64_t length) {
        uint64_t first_page = base / PAGE_SIZE;
        uint64_t count = (length + PAGE_SIZE - 1) / PAGE_SIZE;
        for (uint64_t i = 0; i < count; i++) {
            bitmap_set(first_page + i);
        }
    }

 // src/kernel/pmm.cpp

    uint64_t alloc_page() {
        // --- FIX: Start at 1 to avoid Physical Address 0x0 ---
        // Page 0 is often reserved/cursed. Never hand it out.
        uint64_t total_pages = bitmap_size * 8;
        for (uint64_t i = 1; i < total_pages; i++) { 
             if (!bitmap_test(i)) {
                 bitmap_set(i);
                 return i * 4096; // Return 0x1000, 0x2000, etc.
             }
        }
        return 0; // Out of Memory
    }

    void free_page(uint64_t address) {
        bitmap_unset(address / PAGE_SIZE);
    }
}
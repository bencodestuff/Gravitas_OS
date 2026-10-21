#include "kernel/core/pmm.h"
#include "kernel/klib/klib.h"

extern volatile struct limine_hhdm_request hhdm_request;

namespace PMM {
    static uint8_t* bitmap = nullptr;
    static uint64_t bitmap_size = 0;
    static uint64_t highest_addr = 0;

    // Frame accounting (maintained by alloc/free, seeded by init).
    static uint64_t free_frames_count = 0;
    static uint64_t total_frames_count = 0;

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
        
        kprint("[PMM] HHDM Offset: ");
        kprint_hex(hhdm_offset);
        kprint("\n");

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
        // One byte covers eight frames, so round up: the tail frames of an
        // unaligned total still need a bit, and truncating the byte count made
        // bitmap_set/test touch a byte past the end for them.
        bitmap_size = (total_pages + 7) / 8;
        
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

        kprint("[PMM] Bitmap VIRTUAL Address: ");
        kprint_hex((uint64_t)bitmap);
        kprint("\n");

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
        // Keep this in step with HEAP_SIZE in main.cpp.
        reserve_range(0x08000000, 0x2000000); // 32MB for the kernel heap

        // Seed the free-frame counter from the finished bitmap. The rounded-up
        // bitmap has spare bits past the real frames; they stay set (used), so
        // the count is taken from the actual page total, not the byte count.
        total_frames_count = total_pages;
        free_frames_count = 0;
        for (uint64_t i = 0; i < total_frames_count; i++) {
            if (!bitmap_test(i)) free_frames_count++;
        }

        kprint("[PMM] Initialized successfully.\n");
    }

    uint64_t free_frames() { return free_frames_count; }
    uint64_t total_frames() { return total_frames_count; }

    // Mark a physical range as permanently in use
    void reserve_range(uint64_t base, uint64_t length) {
        uint64_t first_page = base / PAGE_SIZE;
        uint64_t count = (length + PAGE_SIZE - 1) / PAGE_SIZE;
        for (uint64_t i = 0; i < count; i++) {
            bitmap_set(first_page + i);
        }
    }

    uint64_t alloc_page() {
        // --- FIX: Start at 1 to avoid Physical Address 0x0 ---
        // Page 0 is often reserved/cursed. Never hand it out.
        uint64_t total_pages = bitmap_size * 8;
        for (uint64_t i = 1; i < total_pages; i++) { 
             if (!bitmap_test(i)) {
                 bitmap_set(i);
                 if (free_frames_count > 0) free_frames_count--;
                 return i * 4096; // Return 0x1000, 0x2000, etc.
             }
        }
        return 0; // Out of Memory
    }

    void free_page(uint64_t address) {
        uint64_t i = address / PAGE_SIZE;
        // Frame 0 is reserved and never allocated, so a 0 here means the caller
        // passed on the result of a failed allocation; drop it rather than
        // clearing a reserved frame.
        if (i == 0) return;
        if (!bitmap_test(i)) return; // already free: don't over-count
        bitmap_unset(i);
        free_frames_count++;
    }
}
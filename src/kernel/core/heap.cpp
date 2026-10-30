#include "kernel/core/heap.h"
#include "kernel/klib/klib.h"


namespace Heap {

    struct SegmentHeader {
        size_t length;         // How big is this chunk (excluding header)
        SegmentHeader* next;   // Pointer to next segment
        SegmentHeader* prev;   // Pointer to previous segment
        bool is_free;          // Is this chunk available?
        uint8_t reserved[7];   // explicit: keeps 'magic' 8-byte aligned
        uint64_t magic;        // Safety check (0xDEADBEEF)
        // Explicit trailing padding, not compiler padding: sizeof must be a
        // multiple of the payload alignment so the next header stays aligned.
        uint8_t padding[8];
    };

    // C++ operator new promises __STDCPP_DEFAULT_NEW_ALIGNMENT__, which is 16
    // on x86-64. The header starts 16-aligned and is a multiple of 16 bytes,
    // and every request is rounded up to 16, so every payload and every later
    // header is 16-aligned.
    constexpr size_t PAYLOAD_ALIGN = 16;
    static_assert(sizeof(SegmentHeader) % PAYLOAD_ALIGN == 0,
                  "the heap header must preserve payload alignment");

    SegmentHeader* first_segment;

    // Allocation accounting: 'used' is the sum of live segment payloads,
    // 'peak' the high-water mark, 'total' the heap size.
    static uint64_t used_bytes = 0;
    static uint64_t peak_bytes = 0;
    static uint64_t total_bytes = 0;

    void init(void* start_addr, size_t size) {
        // The first payload is start_addr + sizeof(SegmentHeader), so the base
        // must itself be 16-aligned. main.cpp passes HEAP_PHYS_BASE through
        // the HHDM, both page-aligned, which satisfies this.
        if (((uint64_t)start_addr & (PAYLOAD_ALIGN - 1)) != 0)
            kprint("[Heap] WARNING: heap base is not 16-byte aligned!\n");

        // Create the first huge segment covering the whole heap
        first_segment = (SegmentHeader*)start_addr;
        first_segment->length = size - sizeof(SegmentHeader);
        first_segment->next = nullptr;
        first_segment->prev = nullptr;
        first_segment->is_free = true;
        first_segment->magic = 0xDEADBEEF;

        used_bytes = 0;
        peak_bytes = 0;
        total_bytes = size;

        kprint("[Heap] Initialized Linked List Allocator.\n");
    }

    uint64_t used() { return used_bytes; }
    uint64_t peak() { return peak_bytes; }
    uint64_t total() { return total_bytes; }

    void* malloc(size_t size) {
        // Round the request up so the payload is 16-byte aligned like C++
        // operator new requires. A request whose rounding wraps would produce
        // a tiny block for a huge object, so refuse it outright.
        if (size > (size_t)-1 - (PAYLOAD_ALIGN - 1)) {
            kprint("[Heap] OOM (Out of Memory)!\n");
            return nullptr;
        }
        size = (size + (PAYLOAD_ALIGN - 1)) & ~(size_t)(PAYLOAD_ALIGN - 1);

        SegmentHeader* current = first_segment;
        
        // Walk the list to find a free spot
        while (current != nullptr) {
            if (current->is_free && current->length >= size) {
                // FOUND ONE!
                
                // Do we have enough space to split it? 
                // (Requested Size + Header Size + Minimal Remaining space)
                if (current->length > size + sizeof(SegmentHeader) + 32) {
                    
                    // 1. Create a new header for the remaining free space
                    SegmentHeader* new_segment = (SegmentHeader*)( (uint64_t)current + sizeof(SegmentHeader) + size );
                    
                    new_segment->is_free = true;
                    new_segment->magic = 0xDEADBEEF;
                    new_segment->length = current->length - size - sizeof(SegmentHeader);
                    new_segment->next = current->next;
                    new_segment->prev = current;
                    
                    // 2. Update the neighbors
                    if (current->next != nullptr) {
                        current->next->prev = new_segment;
                    }
                    current->next = new_segment;
                    
                    // 3. Shrink the current segment
                    current->length = size;
                }

                current->is_free = false;
                
                used_bytes += current->length;
                if (used_bytes > peak_bytes) peak_bytes = used_bytes;

                // Return memory area AFTER the header
                return (void*)( (uint64_t)current + sizeof(SegmentHeader) );
            }
            current = current->next;
        }

        kprint("[Heap] OOM (Out of Memory)!\n");
        return nullptr;
    }

    void free(void* ptr) {
        if (ptr == nullptr) return; // delete nullptr is legal

        // Go back from the pointer to find the header
        SegmentHeader* segment = (SegmentHeader*)((uint64_t)ptr - sizeof(SegmentHeader));
        
        if (segment->magic != 0xDEADBEEF) {
            kprint("[Heap] CORRUPTION DETECTED: Bad magic number in free!\n");
            return;
        }

        used_bytes -= segment->length;
        segment->is_free = true;

        // MERGE: Combine with Next if free
        if (segment->next != nullptr && segment->next->is_free) {
            segment->length += sizeof(SegmentHeader) + segment->next->length;
            segment->next = segment->next->next;
            if (segment->next != nullptr) segment->next->prev = segment;
        }

        // MERGE: Combine with Prev if free
        if (segment->prev != nullptr && segment->prev->is_free) {
            segment->prev->length += sizeof(SegmentHeader) + segment->length;
            segment->prev->next = segment->next;
            if (segment->next != nullptr) segment->next->prev = segment->prev;
        }
    }
}

// Standard C++ Overrides
void* operator new(size_t size) {
    return Heap::malloc(size);
}

void* operator new[](size_t size) {
    return Heap::malloc(size);
}

void operator delete(void* p) {
    Heap::free(p);
}

void operator delete(void* p, size_t) { // Required for C++14 sizing
    Heap::free(p);
}

void operator delete[](void* p) {
    Heap::free(p);
}
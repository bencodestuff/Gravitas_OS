#include "kernel/core/heap.h"
#include "kernel/klib/klib.h"


namespace Heap {

    struct SegmentHeader {
        size_t length;         // How big is this chunk (excluding header)
        SegmentHeader* next;   // Pointer to next segment
        SegmentHeader* prev;   // Pointer to previous segment
        bool is_free;          // Is this chunk available?
        uint64_t magic;        // Safety check (0xDEADBEEF)
    };

    SegmentHeader* first_segment;

    void init(void* start_addr, size_t size) {
        // Create the first huge segment covering the whole heap
        first_segment = (SegmentHeader*)start_addr;
        first_segment->length = size - sizeof(SegmentHeader);
        first_segment->next = nullptr;
        first_segment->prev = nullptr;
        first_segment->is_free = true;
        first_segment->magic = 0xDEADBEEF;
        
        kprint("[Heap] Initialized Linked List Allocator.\n");
    }

    void* malloc(size_t size) {
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
                
                // Return memory area AFTER the header
                return (void*)( (uint64_t)current + sizeof(SegmentHeader) );
            }
            current = current->next;
        }

        kprint("[Heap] OOM (Out of Memory)!\n");
        return nullptr;
    }

    void free(void* ptr) {
        // Go back from the pointer to find the header
        SegmentHeader* segment = (SegmentHeader*)((uint64_t)ptr - sizeof(SegmentHeader));
        
        if (segment->magic != 0xDEADBEEF) {
            kprint("[Heap] CORRUPTION DETECTED: Bad magic number in free!\n");
            return;
        }

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
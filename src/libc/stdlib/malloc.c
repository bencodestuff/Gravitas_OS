#include <stdlib.h>

#include <grv/syscall.h>

/* --- Simple first-fit allocator over the program break ---
   Layout: [chunk header][usable bytes][chunk header][usable bytes]...
   The break starts at 0x8000000 (128MB, userland VA space) and grows
   upwards via the brk syscall. Headers are 16 bytes, allocations are
   16-byte aligned. Free merges with the next chunk only (backward merge
   is omitted for now — this is a foundation, not a production malloc). */

typedef struct chunk {
    size_t size;      // usable bytes in this chunk (not counting header)
    int    used;
    struct chunk* next;
} chunk_t;

#define CHUNK_HDR_SIZE 16                 // sizeof rounded up to alignment
#define ALIGN16(x) (((x) + 15) & ~(size_t)15)

static chunk_t* first_chunk = NULL;
static void*    current_brk = NULL;

void* malloc(size_t size) {
    if (size == 0) size = 1;
    size = ALIGN16(size);

    // First-fit walk
    for (chunk_t* c = first_chunk; c; c = c->next) {
        if (!c->used && c->size >= size) {
            c->used = 1;
            return (void*)(c + 1);
        }
    }

    // No fit: grow the break
    if (current_brk == NULL) {
        current_brk = grv_brk((void*)0);
        if ((long)current_brk == -1) return NULL;
        if (current_brk == (void*)0) current_brk = (void*)0x8000000;
    }

    size_t need = CHUNK_HDR_SIZE + size;
    chunk_t* c = (chunk_t*)grv_brk((char*)current_brk + need);
    if ((long)c == -1 || c == current_brk) return NULL; // brk refused

    c->size = size;
    c->used = 1;
    c->next = NULL;

    if (first_chunk == NULL) {
        first_chunk = c;
    } else {
        chunk_t* end = first_chunk;
        while (end->next) end = end->next;
        end->next = c;
    }

    current_brk = (char*)c + CHUNK_HDR_SIZE + size;
    return (void*)(c + 1);
}

void free(void* ptr) {
    if (ptr == NULL) return;
    chunk_t* c = (chunk_t*)ptr - 1;
    c->used = 0;
    // TODO: merge adjacent free chunks
}

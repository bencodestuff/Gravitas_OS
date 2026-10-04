#include <stdlib.h>

#include <grv/syscall.h>

/* --- Simple first-fit allocator over the program break ---
   Layout: [chunk header][usable bytes][chunk header][usable bytes]...
   The break starts at 0x8000000 (128MB, userland VA space) and grows
   upwards via the brk syscall. The header is exactly 16 bytes and
   allocations are 16-byte aligned, so a payload never runs into the next
   header. Free merges with the next chunk only (backward merge is omitted
   for now — this is a foundation, not a production malloc). */

typedef struct chunk {
    size_t size;          // usable bytes (16-aligned); bit 0 = used
    struct chunk* next;
} chunk_t;

#define CHUNK_HDR_SIZE 16                 // == sizeof(chunk_t)
#define CHUNK_USED     1ul
#define CHUNK_SIZE(c)  ((c)->size & ~CHUNK_USED)
#define ALIGN16(x)     (((x) + 15) & ~(size_t)15)

static chunk_t* first_chunk = NULL;
static void*    current_brk = NULL;

void* malloc(size_t size) {
    if (size == 0) size = 1;
    size = ALIGN16(size);

    // First-fit walk
    for (chunk_t* c = first_chunk; c; c = c->next) {
        if (!(c->size & CHUNK_USED) && CHUNK_SIZE(c) >= size) {
            c->size |= CHUNK_USED;
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
    void* new_break = grv_brk((char*)current_brk + need);
    if ((long)new_break == -1 || new_break == current_brk) return NULL; // brk refused

    // The chunk begins at the OLD break (grv_brk returns the NEW break —
    // using it as the chunk base would place the payload past the mapped
    // region)
    chunk_t* c = (chunk_t*)current_brk;
    c->size = size | CHUNK_USED;
    c->next = NULL;

    if (first_chunk == NULL) {
        first_chunk = c;
    } else {
        chunk_t* end = first_chunk;
        while (end->next) end = end->next;
        end->next = c;
    }

    current_brk = new_break;
    return (void*)(c + 1);
}

void free(void* ptr) {
    if (ptr == NULL) return;
    chunk_t* c = (chunk_t*)ptr - 1;
    c->size &= ~CHUNK_USED;
    // TODO: merge adjacent free chunks
}

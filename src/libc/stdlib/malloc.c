#include <stdlib.h>

#include <grv/syscall.h>

/* --- First-fit allocator over the program break ---
   Layout: [16-byte header][payload][16-byte header][payload]...
   The header keeps the payload size (16-aligned; bit 0 = used) and the next
   chunk, so a payload never runs into the next header. The chunks form an
   address-ordered list, which keeps coalescing simple:

     - free() merges with the chunk before and after it, so neighbouring
       frees become one usable block again;
     - a reused free chunk is split when there is enough surplus to matter,
       so a big freed buffer can serve many small requests instead of being
       handed out whole;
     - realloc absorbs the free chunk(s) that follow before falling back to
       allocate-and-copy.

   Backward coalescing finds the predecessor by walking the list. That is
   O(n) per free, but it keeps the per-allocation overhead at 16 bytes
   (a boundary tag/footer would add 8 bytes to every allocation).

   Failure policy: malloc/calloc/realloc return NULL when the break cannot
   grow; callers must check. No allocation is silently dropped.

   The break is never shrunk on free: the kernel keeps pages mapped when brk
   moves down, so it would return no memory — the free list is what makes
   freed space reusable. */

typedef struct chunk {
    size_t size;          // payload bytes (16-aligned); bit 0 = used
    struct chunk* next;
} chunk_t;

#define CHUNK_HDR_SIZE 16                 // == sizeof(chunk_t)
#define CHUNK_USED     1ul
#define CHUNK_SIZE(c)  ((c)->size & ~CHUNK_USED)
#define ALIGN16(x)     (((x) + 15) & ~(size_t)15)

// Smallest surplus worth splitting off (header + one 16-byte payload).
#define MIN_SPLIT      32

static chunk_t* first_chunk = NULL;
static void*    current_brk = NULL;

// The chunk immediately before 'c' in address order, or NULL.
static chunk_t* prev_chunk(chunk_t* c) {
    if (c == first_chunk) return NULL;
    for (chunk_t* p = first_chunk; p; p = p->next)
        if (p->next == c) return p;
    return NULL;
}

// Coalesce 'c' with every free chunk directly after it.
static void merge_forward(chunk_t* c) {
    while (c->next && !(c->next->size & CHUNK_USED)) {
        c->size = CHUNK_SIZE(c) + CHUNK_HDR_SIZE + CHUNK_SIZE(c->next);
        c->next = c->next->next;
    }
}

// Hand out 'size' bytes of 'c' and return the surplus (if any) to the list.
// 'c' ends up marked used.
static void split(chunk_t* c, size_t size) {
    size_t csize = CHUNK_SIZE(c);
    if (csize < size + MIN_SPLIT) {
        c->size = csize | CHUNK_USED; // too little left over to bother
        return;
    }
    chunk_t* tail = (chunk_t*)((char*)(c + 1) + size);
    tail->size = csize - size - CHUNK_HDR_SIZE; // free
    tail->next = c->next;
    c->next = tail;
    c->size = size | CHUNK_USED;
    merge_forward(tail);
}

void* malloc(size_t size) {
    if (size == 0) size = 1;
    size = ALIGN16(size);

    // First fit over the free list.
    for (chunk_t* c = first_chunk; c; c = c->next) {
        if (c->size & CHUNK_USED) continue;
        if (CHUNK_SIZE(c) < size) continue;
        split(c, size);
        return (void*)(c + 1);
    }

    // No fit: grow the break.
    if (current_brk == NULL) {
        current_brk = grv_brk((void*)0);
        if ((long)current_brk == -1) return NULL;
        if (current_brk == (void*)0) current_brk = (void*)0x8000000;
    }

    size_t need = CHUNK_HDR_SIZE + size;
    void* new_break = grv_brk((char*)current_brk + need);
    if ((long)new_break == -1 || new_break == current_brk) return NULL; // refused

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

    chunk_t* p = prev_chunk(c);
    if (p && !(p->size & CHUNK_USED)) {
        // Fold c into the free chunk before it, then coalesce forward.
        p->size = CHUNK_SIZE(p) + CHUNK_HDR_SIZE + CHUNK_SIZE(c);
        p->next = c->next;
        merge_forward(p);
    } else {
        merge_forward(c);
    }
}

// Grow in place when possible (absorbing free neighbours or extending the
// break), else allocate, copy and free. Shrinking returns the surplus to the
// free list.
void* realloc(void* ptr, size_t size) {
    if (ptr == NULL) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }

    chunk_t* c = (chunk_t*)ptr - 1;
    size_t old = CHUNK_SIZE(c);
    size_t need = ALIGN16(size);

    if (need <= old) {
        split(c, need); // shrink in place; the tail stays allocatable
        return ptr;
    }

    // Absorb the free chunk(s) directly after us when that is enough.
    size_t avail = old;
    for (chunk_t* n = c->next; n && !(n->size & CHUNK_USED); n = n->next)
        avail += CHUNK_HDR_SIZE + CHUNK_SIZE(n);
    if (avail >= need) {
        while (c->next && !(c->next->size & CHUNK_USED)) {
            c->size = CHUNK_SIZE(c) + CHUNK_HDR_SIZE + CHUNK_SIZE(c->next);
            c->next = c->next->next;
        }
        c->size = avail | CHUNK_USED;
        split(c, need);
        return ptr;
    }

    if (c->next == NULL && current_brk != NULL) {
        void* nb = grv_brk((char*)current_brk + (need - old));
        if ((long)nb != -1 && nb != current_brk) {
            current_brk = nb;
            c->size = need | CHUNK_USED;
            return ptr;
        }
    }

    void* np = malloc(size);
    if (!np) return NULL;
    unsigned char* d = (unsigned char*)np;
    unsigned char* s = (unsigned char*)ptr;
    size_t copy = old < need ? old : need;
    for (size_t i = 0; i < copy; i++) d[i] = s[i];
    free(ptr);
    return np;
}

void* calloc(size_t count, size_t size) {
    if (size != 0 && count > (size_t)-1 / size) return NULL; // overflow
    size_t total = count * size;
    void* p = malloc(total);
    if (!p) return NULL;
    unsigned char* b = (unsigned char*)p;
    for (size_t i = 0; i < total; i++) b[i] = 0;
    return p;
}
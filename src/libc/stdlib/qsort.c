#include <stdlib.h>

// Simple quicksort (last element as pivot). Good enough for the small
// arrays userland sorts; worst case is O(n^2) on already-sorted input.

static void swap(char* a, char* b, size_t n) {
    for (size_t i = 0; i < n; i++) { char t = a[i]; a[i] = b[i]; b[i] = t; }
}

static void qs(char* base, size_t n, size_t size,
               int (*cmp)(const void*, const void*)) {
    if (n < 2) return;
    char* pivot = base + (n - 1) * size;
    size_t i = 0;
    for (size_t j = 0; j < n - 1; j++) {
        if (cmp(base + j * size, pivot) < 0) {
            if (i != j) swap(base + i * size, base + j * size, size);
            i++;
        }
    }
    swap(base + i * size, pivot, size);
    qs(base, i, size, cmp);
    qs(base + (i + 1) * size, n - i - 1, size, cmp);
}

void qsort(void* base, size_t count, size_t size,
           int (*compare)(const void*, const void*)) {
    qs((char*)base, count, size, compare);
}

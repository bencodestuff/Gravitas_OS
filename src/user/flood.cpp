#include <stdio.h>
#include <grv/syscall.h>

// flood — prints 150 numbered lines, forcing many scroll events on any
// realistic console (the 8x16 font gives ~50 rows on this framebuffer).
// The visual test: the console keeps its last screenful and stays
// responsive; the test suite guards that all 150 lines go through.

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    for (int i = 1; i <= 150; i++) {
        printf("flood line %d — scrolling test\n", i);
    }
    printf("flood done\n");
    return 0;
}

#include <stdio.h>
#include <grv/syscall.h>

// flood — prints 150 numbered lines. At 8px per row on a 768px-tall
// framebuffer (~96 rows), this forces ~55 scroll events. The visual
// test: the console keeps its last screenful and stays responsive.

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    for (int i = 1; i <= 150; i++) {
        printf("flood line %d — scrolling test\n", i);
    }
    printf("flood done\n");
    return 0;
}

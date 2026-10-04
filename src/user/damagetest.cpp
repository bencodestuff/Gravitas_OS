#include <stdio.h>
#include <grv/syscall.h>

// damagetest — M6 acceptance for damage-tracked compositing.
//
// Prints a text banner, then creates a solid surface over it, moves it
// around, and destroys it. Visually: the window appears and moves, and
// each move restores the text/background it uncovered (no ghosting, no
// stale pixels). It samples the compositor's pixel counter around ONLY the
// surface operations (no console output in between), so the smoke harness
// can assert the window repainted far less than a full-screen repaint per
// operation regardless of how full the screen is.

static void fill(struct grv_surface_info* s, unsigned int color) {
    unsigned int* px = (unsigned int*)s->address;
    unsigned long n = s->width * s->height;
    for (unsigned long i = 0; i < n; i++) px[i] = color;
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    // Text that the window will cover and uncover.
    printf("damagetest: cover me (A)\n");
    printf("damagetest: cover me (B)\n");

    struct grv_surface_info s;
    int id = grv_surface_create(240, 160, &s);
    if (id < 0) { printf("damagetest: FAIL create\n"); return 1; }

    struct grv_sysinfo a, b;
    grv_sysinfo(&a); // window-lifecycle damage measured from here

    fill(&s, 0xFFFF00FF);            // opaque magenta
    grv_surface_move(id, 400, 200);
    grv_surface_commit();
    grv_sleep(700);

    grv_surface_move(id, 120, 420);  // down-left, over the banner text
    grv_surface_commit();
    grv_sleep(700);

    grv_surface_move(id, 900, 60);   // top-right, over empty background
    grv_surface_commit();
    grv_sleep(700);

    if (grv_surface_destroy(id) != 0) {
        printf("damagetest: FAIL destroy\n");
        return 1;
    }
    grv_sleep(300);

    grv_sysinfo(&b);
    unsigned long dmg = (unsigned long)(b.comp_pixels - a.comp_pixels);
    printf("damagetest: window damage %lu px\n", dmg);
    printf("damagetest done\n");
    return 0;
}


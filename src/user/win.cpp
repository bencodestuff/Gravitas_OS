#include <stdio.h>
#include <grv/syscall.h>
#include <grv/text.h>

// win — a minimal windowed client for the WM demo.
//
// Creates a surface, paints its content, and shows the last key it received
// through GETC. Because the kernel routes keys only to the focused task, the
// window must be clicked (focused by the WM) before it reacts.

#define W 360
#define H 220

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    struct grv_surface_info si;
    int id = grv_surface_create(W, H, &si);
    if (id < 0) { printf("win: surface create failed\n"); return 1; }

    unsigned int* px = (unsigned int*)si.address;
    struct grv_font font;
    int fok = grv_font_load(&font, "/fonts/bizcat.psf");

    for (int i = 0; i < W * H; i++) px[i] = 0xFF203040;
    if (fok) {
        grv_text_draw(&font, px, W, W, H, 12, 12, "win.elf", 0xFFFFFFFF);
        grv_text_draw(&font, px, W, W, H, 12, 40, "click, then type", 0xFFA0C0E0);
        grv_text_draw(&font, px, W, W, H, 12, 80, "key:", 0xFFA0C0E0);
    }
    grv_surface_commit();

    printf("win: window ready (surface #%d)\n", id);

    while (1) {
        char c = grv_getc();          // blocks until this task is focused
        char last = (c >= 32 && c < 127) ? c : '?';

        // Clear and redraw the key cell, then commit just that rect.
        int kh = fok ? (int)font.height : 8;
        for (int y = 80; y < 80 + kh; y++)
            for (int x = 60; x < 68; x++) px[y * W + x] = 0xFF203040;
        if (fok) {
            char s[2] = { last, 0 };
            grv_text_draw(&font, px, W, W, H, 60, 80, s, 0xFFFFE080);
        }
        grv_surface_commit_rect(58, 78, 14, kh + 4);

        printf("win: key '%c'\n", last);
    }
    return 0;
}

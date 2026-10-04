#include <stdio.h>
#include <grv/syscall.h>
#include <grv/text.h>
#include <grv/input.h>

// win — a minimal windowed client for the WM demo.
//
// Creates a surface, paints its content, and reacts to input events: it
// shows the last key it received and marks where it was clicked (in
// window-local coordinates). Input arrives as messages once it enables
// event mode — the WM forwards mouse events to the window under the
// pointer, and the kernel delivers keys to whichever window is focused.

#define W 360
#define H 220

static unsigned int* px;

static void mark(int x, int y, unsigned int color) {
    for (int yy = y - 3; yy <= y + 3; yy++) {
        if (yy < 0 || yy >= H) continue;
        for (int xx = x - 3; xx <= x + 3; xx++) {
            if (xx < 0 || xx >= W) continue;
            px[yy * W + xx] = color;
        }
    }
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    struct grv_surface_info si;
    int id = grv_surface_create(W, H, &si);
    if (id < 0) { printf("win: surface create failed\n"); return 1; }

    px = (unsigned int*)si.address;
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

    grv_input_mode(1); // receive keys and mouse as messages

    while (1) {
        struct grv_msg m;
        if (grv_recv(&m, 1) != 0) continue;

        if (m.type == GRV_EV_KEY) {
            long v = m.a;
            char last = (v >= 32 && v < 127) ? (char)v : '?';
            int kh = fok ? (int)font.height : 8;
            for (int y = 80; y < 80 + kh; y++)
                for (int x = 60; x < 68; x++) px[y * W + x] = 0xFF203040;
            if (fok) {
                char s[2] = { last, 0 };
                grv_text_draw(&font, px, W, W, H, 60, 80, s, 0xFFFFE080);
            }
            grv_surface_commit_rect(58, 78, 14, kh + 4);
            printf("win: key '%c'\n", last);

        } else if (m.type == GRV_EV_MOUSE) {
            if (m.a & GRV_BTN_LEFT) { // a press
                int x = grv_mouse_x(m.b);
                int y = grv_mouse_y(m.b);
                mark(x, y, 0xFFFF8040);
                grv_surface_commit_rect(x - 4, y - 4, 8, 8);
                printf("win: click %d,%d\n", x, y);
            }
        }
    }
    return 0;
}

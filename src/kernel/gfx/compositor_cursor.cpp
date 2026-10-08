#include "kernel/gfx/compositor_internal.h"
#include "kernel/dev/mouse.h"
#include "kernel/gfx/cursor.h"

// --- Compositor: the pointer sprite ---
//
// The kernel owns the authoritative pointer position; this unit clamps it to
// the screen and stamps the 16x16 sprite into whatever buffer the painter is
// currently composing into (g_paint). present() remembers where the sprite was
// so the next frame can repair it.

namespace Compositor {

int last_cursor_x = -10000, last_cursor_y = -10000;
bool cursor_seen = false;

void cursor_pos(int* x, int* y) {
    int mx = Mouse::cursor_x(), my = Mouse::cursor_y();
    // Clamp the HOTSPOT, not the sprite: the pointer must be able to reach the
    // screen edge (a window flush to it has to stay grabbable), so a sprite
    // that hangs over is clipped instead.
    if (mx > (int)fb_w - 1) mx = (int)fb_w - 1;
    if (my > (int)fb_h - 1) my = (int)fb_h - 1;
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    Mouse::apply_position_clamp(mx, my);
    *x = mx; *y = my;
}

void draw_cursor() {
    int mx, my;
    cursor_pos(&mx, &my);
    for (int cy = 0; cy < CURSOR_H; cy++) {
        int y = my + cy;
        if (y < 0 || y >= (int)fb_h) continue;   // clipped at the bottom
        uint32_t* dst = g_paint + (uint64_t)y * fb_pitch_px + mx;
        for (int cx = 0; cx < CURSOR_W; cx++) {
            int x = mx + cx;
            if (x < 0 || x >= (int)fb_w) continue; // clipped at the right
            uint8_t v = cursor_sprite16[cy][cx];
            if (v == 0) continue;
            dst[cx] = (v == 1) ? 0x00000000 : 0x00FFFFFF;
        }
    }
    last_cursor_x = mx;
    last_cursor_y = my;
    cursor_seen = true;
}

} // namespace Compositor

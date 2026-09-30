#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <grv/syscall.h>

// plasma — the classic demoscene effect, mouse-reactive.
//
// Sum of sines: horizontal + vertical bands, ripples radiating from a
// center that FOLLOWS THE MOUSE, and a time pulse. Clicks push a palette
// phase shift. Everything integer: precomputed sine table (1024 entries),
// palette of 256 colors. Exits on 3 left-clicks.

// 256-entry palette: hue sweep -> RGB, fixed-point HSV-ish
static unsigned int* palette = 0;

static void build_palette(int phase) {
    for (int i = 0; i < 256; i++) {
        int h = (i * 360 / 256 + phase) % 360; // degrees
        // HSV->RGB, S=1, V=1 (integer approximation via sextant)
        int region = h / 60;
        int f = (h % 60) * 255 / 60;
        int r, g, b;
        switch (region % 6) {
            case 0: r = 255; g = f;     b = 0;     break;
            case 1: r = 255 - f; g = 255; b = 0;     break;
            case 2: r = 0;     g = 255; b = f;     break;
            case 3: r = 0;     g = 255 - f; b = 255; break;
            case 4: r = f;     g = 0;     b = 255; break;
            default: r = 255; g = 0;     b = 255 - f; break;
        }
        palette[i] = 0xFF000000 | ((unsigned int)r << 16) | ((unsigned int)g << 8) | (unsigned int)b;
    }
}

// clamp to [lo, hi]
static int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    // Surface-based: the compositor owns the real framebuffer; we render
    // into our own surface and commit each frame.
    struct grv_surface_info surf;
    int sid = grv_surface_create(1280, 800, &surf);
    if (sid < 0) {
        printf("plasma: surface_create failed\n");
        return 1;
    }

    math_init(); // libm: build the sine table
    palette = (unsigned int*)malloc(256 * sizeof(unsigned int));
    build_palette(0);

    unsigned int* fbmem = (unsigned int*)surf.address;

    printf("plasma: %dx%d — move the mouse, click 3x to exit\n",
           (int)surf.width, (int)surf.height);

    // Effect state: ripple center follows the mouse (starts center)
    int cx = (int)surf.width / 2, cy = (int)surf.height / 2;
    int palette_phase = 0;
    int frame = 0;
    int left_clicks = 0;
    bool prev_left = false;
    bool left_down = false;

    // FPS accounting
    int frames_done = 0;
    int fps_total = 0;
    unsigned long ticks_at_window_start = 0;

    while (left_clicks < 3) {
        // --- input: mouse ---
        struct grv_mouse_packet pkt;
        while (grv_mouse_read(&pkt)) {
            cx = clampi(cx + pkt.dx, 0, (int)surf.width - 1);
            cy = clampi(cy + pkt.dy, 0, (int)surf.height - 1);

            bool left = (pkt.buttons & 1) != 0;
            if (left && !prev_left) {         // rising edge = click
                left_clicks++;
                palette_phase += 60;          // click pulses the palette
                build_palette(palette_phase);
            }
            prev_left = left;
            left_down = left;
        }

        // --- render one frame ---
        // t advances fast enough to see motion: the moving phase rides
        // inside all three terms so the whole field flows against the
        // mouse ripples.
        //
        // Per-frame precompute: x and y terms cost 2K table lookups per
        // frame instead of 4M — the only per-pixel work left is the
        // mouse-ripple term. (Per-pixel sines were ~3fps real under TCG.)
        int t = (frame * 14) & 1023;
        static int xterm[4096], yterm[4096];
        int W = (int)surf.width, H = (int)surf.height;
        for (int x = 0; x < W; x++)
            xterm[x] = isin((x * 8 + t * 3) & 1023);
        for (int y = 0; y < H; y++)
            yterm[y] = isin((y * 12 - t * 2) & 1023);

        unsigned int* row = fbmem;
        for (int y = 0; y < H; y++) {
            int dy = y - cy;
            int yterm_v = yterm[y];
            unsigned int* px = row;
            for (int x = 0; x < W; x++) {
                int dx = x - cx;
                // ripples from the mouse center: manhattan distance — cheap
                // and looks right; the time term rides inside the phase so
                // ripples flow outward
                int dist = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
                int v = xterm[x]
                      + yterm_v
                      + isin((dist * 20 - t * 4) & 1023)
                      + isin(t);
                // v in [-2048, 2048] -> 0..255
                int idx = ((v + 2048) * 255) / 4096;
                *px++ = palette[idx & 0xFF];
            }
            row += surf.width; // surface is tightly packed (pitch == width*4)
        }

        grv_surface_commit(); // hand the frame to the compositor

        frame++;
        frames_done++;

        // FPS: honest wall-clock via timer ticks (syscall 17). The old
        // counter assumed 20ms/loop which sleep-instant-return broke.
        if (frames_done >= 50) {
            unsigned long now = grv_ticks();
            unsigned long ms = (ticks_at_window_start && now > ticks_at_window_start)
                             ? (now - ticks_at_window_start) * 20 : 1;
            printf("plasma: %d frames in %dms (~%d fps) mouse %d,%d %s\n",
                   frames_done, (int)ms,
                   (int)((unsigned long)frames_done * 1000 / (ms ? ms : 1)),
                   cx, cy, left_down ? "[L]" : "");
            fps_total += frames_done;
            frames_done = 0;
            ticks_at_window_start = now;
        }

        grv_sleep(20); // pace to ~50Hz max
    }

    printf("plasma: %d frames total. bye!\n", fps_total + frames_done);
    return 0;
}

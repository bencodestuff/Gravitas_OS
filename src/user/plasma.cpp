#include <stdio.h>
#include <stdlib.h>
#include <grv/syscall.h>

// plasma — the classic demoscene effect, mouse-reactive.
//
// Sum of sines: horizontal + vertical bands, ripples radiating from a
// center that FOLLOWS THE MOUSE, and a time pulse. Clicks push a palette
// phase shift. Everything integer: precomputed sine table (1024 entries),
// palette of 256 colors. Exits on 3 left-clicks.

// --- fixed-point sine table: sin(x) * 512 for x in [0, 1024) mapping to
// a full 2*pi cycle ---
static int* sintab = 0;
#define SIN_SCALE 512
#define SIN_MASK  1023

static void build_sintab() {
    sintab = (int*)malloc(1024 * sizeof(int));
    // angle 0..2pi over 1024 entries; sin via Taylor series in fixed point
    for (int i = 0; i < 1024; i++) {
        // x = i * 2pi / 1024, in radians-ish: t = i * 0.0061359
        // Taylor: sin(t) = t - t^3/6 + t^5/120 - ... in units of SIN_SCALE
        // Fixed point: t = i (0..1023) representing radians scaled by
        // (2*pi/1024). Work in units where t64 = i * 2pi/1024 * 64 for
        // headroom, then series in t64-space.
        long long t64 = (long long)i * 402; // 2*pi*64/1024 = 0.3927...*64 = 402/1024... simplification: i*402/1024 rad*64
        // do the series in scaled space: s = t (radians * 64 * 1024 / 1024)
        // Simpler: angle in 1/1024 turns: a = i. radians = a * 6.2832/1024.
        // Use scaled radians: r = a * 4/1024 * pi*1024... KISS:
        // precompute with integer Taylor over r = a (0..1023), where
        // real_radians = a * 0.0061359. Series needs radians; scale by 2^16:
        long long R = (long long)i * 411775; // 0.0061359 * 2^16 = 402.0... i * 402
        // R is radians << 16 where radians = i * 2pi/1024. R = i * 402 (=0.0061359*65536=402.03)
        long long Rsq = (R * R) >> 16;           // r^2 << 16
        long long Rsq2 = (Rsq * R) >> 16;        // r^3 << 16
        long long Rsq4 = (Rsq * Rsq) >> 16;      // r^4 << 16
        long long Rsq5 = (Rsq4 * R) >> 16;       // r^5 << 16
        long long Rsq6 = (Rsq4 * Rsq) >> 16;     // r^6 << 16
        long long Rsq7 = (Rsq6 * R) >> 16;       // r^7 << 16
        long long s = R - Rsq2 / 6 + Rsq5 / 120 - Rsq7 / 5040;
        // s is radians << 16; convert to SIN_SCALE units (sin*512):
        // sin ~= s / 65536 * 512 = s / 128
        sintab[i] = (int)(s / 128);
    }
}

// sin(a) where a is in 1/1024-turns units (0..1023), returns sin * 512
static int fsin(int a) {
    return sintab[a & SIN_MASK];
}

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

    struct grv_fb_info fb;
    if (grv_fb_info(&fb) != 0) {
        printf("plasma: no framebuffer\n");
        return 1;
    }
    if (fb.bpp != 32) {
        printf("plasma: want 32bpp, got %d\n", (int)fb.bpp);
        return 1;
    }

    build_sintab();
    palette = (unsigned int*)malloc(256 * sizeof(unsigned int));
    build_palette(0);

    unsigned int* fbmem = (unsigned int*)fb.address;

    printf("plasma: %dx%d — move the mouse, click 3x to exit\n",
           (int)fb.width, (int)fb.height);

    // Effect state: ripple center follows the mouse (starts center)
    int cx = (int)fb.width / 2, cy = (int)fb.height / 2;
    int palette_phase = 0;
    int frame = 0;
    int left_clicks = 0;
    bool prev_left = false;
    bool left_down = false;

    // Frames rendered once per second for the FPS counter
    int fps_count = 0;
    int fps_total = 0;

    while (left_clicks < 3) {
        // --- input: mouse ---
        struct grv_mouse_packet pkt;
        while (grv_mouse_read(&pkt)) {
            cx = clampi(cx + pkt.dx, 0, (int)fb.width - 1);
            cy = clampi(cy + pkt.dy, 0, (int)fb.height - 1);

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
        int t = (frame * 4) & SIN_MASK;       // time pulse
        unsigned int* row = fbmem;
        for (int y = 0; y < (int)fb.height; y++) {
            int dy = y - cy;
            unsigned int* px = row;
            for (int x = 0; x < (int)fb.width; x++) {
                int dx = x - cx;
                // ripples from the mouse center: distance approximated by
                // |dx| + |dy| (manhattan — cheap and looks right)
                int dist = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
                int v = fsin((x * 8) & SIN_MASK)
                      + fsin((y * 12) & SIN_MASK)
                      + fsin((dist * 20) & SIN_MASK)
                      + fsin(t);
                // v in [-2048, 2048] -> 0..255
                int idx = ((v + 2048) * 255) / 4096;
                *px++ = palette[idx & 0xFF];
            }
            row += fb.pitch / 4;
        }

        frame++;
        fps_count++;

        // FPS report once per second (~50 frames)
        if (fps_count >= 50) {
            printf("plasma: %d frames/s (mouse %d,%d %s)\n",
                   fps_count, cx, cy, left_down ? "[L]" : "");
            fps_total += fps_count;
            fps_count = 0;
        }

        grv_sleep(20); // ~50Hz
    }

    printf("plasma: %d frames total. bye!\n", fps_total + fps_count);
    return 0;
}

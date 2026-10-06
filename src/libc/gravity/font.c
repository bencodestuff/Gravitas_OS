#include <gravity/font.h>
#include <grv/syscall.h>
#include <stdlib.h>
#include <stdint.h>

/* PSF2 parsing mirrors the kernel's (gfx/font.cpp), including Bizcat's
   field-order quirk: the generator emitted (charsize, height, width) where
   the spec says (width, height, charsize). Only 8px-wide fonts render. */

static uint32_t rd32(const unsigned char* d, int o) {
    return (uint32_t)d[o]
         | ((uint32_t)d[o + 1] << 8)
         | ((uint32_t)d[o + 2] << 16)
         | ((uint32_t)d[o + 3] << 24);
}

int gravity_font_load(struct gravity_font* f, const char* path) {
    int fd = grv_open(path, GRV_O_RDONLY);
    if (fd < 0) return 0;

    unsigned long cap = 65536;
    unsigned char* buf = (unsigned char*)malloc(cap);
    if (buf == 0) { grv_close(fd); return 0; }

    unsigned long off = 0;
    long got;
    while (off < cap && (got = grv_read_fd(fd, buf + off, cap - off)) > 0) {
        off += (unsigned long)got;
    }
    grv_close(fd);

    if (off < 32 || rd32(buf, 0) != 0x864ab572u) return 0;

    uint32_t headersize = rd32(buf, 8);
    uint32_t length     = rd32(buf, 16);
    uint32_t f20        = rd32(buf, 20);
    uint32_t f24        = rd32(buf, 24);
    uint32_t f28        = rd32(buf, 28);

    uint32_t width = 0, height = 0, charsize = 0;
    if (f20 == 8 && f28 == f24) {         /* spec: (w,h,c) */
        width = f20; height = f24; charsize = f28;
    } else if (f28 == 8 && f20 == f24) {  /* Bizcat: (c,h,w) */
        width = f28; height = f24; charsize = f20;
    } else {
        return 0;
    }

    if (width != 8 || height == 0 || charsize != height) return 0;
    if (length < 128 || headersize < 32) return 0;
    if ((unsigned long)headersize + (unsigned long)length * charsize > off) return 0;

    f->glyphs = buf + headersize;
    f->height = height;
    return 1;
}

void gravity_font_draw(const struct gravity_font* f, unsigned int* px,
                       int stride, int w, int h, int x, int y, const char* s,
                       unsigned int fg) {
    if (f == 0 || f->glyphs == 0) return;
    unsigned gh = f->height;

    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c >= 32 && c <= 126) {
            const unsigned char* g = f->glyphs + (unsigned long)c * gh;
            for (unsigned row = 0; row < gh; row++) {
                int py = y + (int)row;
                if (py < 0 || py >= h) continue;
                unsigned char bits = g[row];
                for (int col = 0; col < 8; col++) {
                    if (!(bits & (0x80 >> col))) continue;
                    int pxx = x + col;
                    if (pxx < 0 || pxx >= w) continue;
                    px[(unsigned long)py * stride + pxx] = fg;
                }
            }
        }
        x += 8;
    }
}

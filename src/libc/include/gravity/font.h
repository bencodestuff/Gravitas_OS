#ifndef _GRAVITY_FONT_H
#define _GRAVITY_FONT_H

#include <sys/cdefs.h>

__BEGIN_DECLS

/* Text rendering for userland: loads the same PSF2 console font the kernel
   uses (files under the fonts directory, e.g. /fonts/bizcat.psf) and blits
   ASCII into a 32bpp surface buffer. */

struct gravity_font {
    const unsigned char* glyphs;  /* 8px-wide glyph bitmaps (row-major) */
    unsigned height;              /* rows per glyph */
};

/* Load an 8px-wide PSF2 font from 'path'. The glyph data is heap-allocated
   and kept for the process lifetime. Returns 1 on success, 0 on failure. */
int gravity_font_load(struct gravity_font* f, const char* path);

/* Draw an ASCII string into a 32bpp buffer 'px' (row stride 'stride' px),
   writing only the set glyph pixels with 'fg'. Clipped to [0,w) x [0,h). */
void gravity_font_draw(const struct gravity_font* f, unsigned int* px,
                       int stride, int w, int h, int x, int y, const char* s,
                       unsigned int fg);

/* Pixel width of a string (8 px per glyph; non-printables advance too). */
static inline int gravity_font_width(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n * 8;
}

__END_DECLS

#endif

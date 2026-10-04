#ifndef _GRAVITY_CANVAS_H
#define _GRAVITY_CANVAS_H

#include <sys/cdefs.h>

__BEGIN_DECLS

/* Gravity canvas: a surface-backed pixel buffer with drawing primitives and
   dirty-rectangle tracking. The surface is the window; the WM decorates and
   positions it. Drawing marks the touched rectangle dirty; gravity_present()
   commits just that rectangle, so a partial redraw is a partial repaint. */

struct gravity_canvas {
    unsigned int* px;   /* 32bpp pixels (the surface's VA) */
    int w, h;           /* pixel size */
    int surface_id;     /* the surface (window) this canvas draws into */
    int dx0, dy0, dx1, dy1; /* dirty bounds, exclusive x1/y1; empty if x1<=x0 */
};

/* Load the default console font (idempotent). Returns 1 if text can render. */
int gravity_init(void);

/* Active font metrics (for centering labels); 8 if no font loaded. */
int gravity_font_height(void);
int gravity_text_width_of(const char* s);

/* Create the w*h window surface and bind the canvas to it. 0 on success. */
int gravity_canvas_init(struct gravity_canvas* c, int w, int h);
void gravity_canvas_done(struct gravity_canvas* c);

/* Mark a rectangle as needing commit. Primitives call this themselves. */
void gravity_invalidate(struct gravity_canvas* c, int x, int y, int w, int h);

void gravity_clear(struct gravity_canvas* c, unsigned int color);
void gravity_fill(struct gravity_canvas* c, int x, int y, int w, int h,
                  unsigned int color);
void gravity_border(struct gravity_canvas* c, int x, int y, int w, int h,
                    unsigned int color);
void gravity_text(struct gravity_canvas* c, int x, int y, const char* s,
                  unsigned int fg);

/* One fixed cell: fill (x,y,8,font_height) with 'bg', then blit glyph 'ch'
   in 'fg'. The building block for a text grid. */
void gravity_cell(struct gravity_canvas* c, int x, int y, unsigned char ch,
                  unsigned int fg, unsigned int bg);

/* Commit the dirty rectangle to the surface (no-op if nothing is dirty). */
void gravity_present(struct gravity_canvas* c);

__END_DECLS

#endif

#ifndef _GRAVITY_TERM_H
#define _GRAVITY_TERM_H

#include <gravity/canvas.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

/* A terminal text grid on a Gravity canvas: a cols x rows cell matrix over a
   bounded scrollback history, plus a printable-byte writer that handles
   \n, \r, \b, \t and auto-wrap. An app feeds it bytes (typically a hosted
   program's output) and renders it; only changed rows are repainted. */

#define GRAVITY_TERM_SCROLL 512 /* scrollback lines kept above the screen */

struct gravity_term {
    int cols, rows;
    int cell_w, cell_h;          /* cell size in pixels */
    unsigned int def_fg, def_bg;
    int capacity;                /* rows + GRAVITY_TERM_SCROLL */
    int used;                    /* lines written (<= capacity) */
    int cur;                     /* current output line (absolute) */
    int cx;                      /* cursor column */
    int view;                    /* scrollback offset (0 = live) */
    unsigned char* chars;        /* [capacity*cols] characters */
    unsigned int* fg;            /* [capacity*cols] foregrounds */
    unsigned char* dirty;        /* [capacity] row needs repaint */
    int all_dirty;               /* set on scroll/view change */
};

/* Allocate the grid. Returns 0 on success, -1 if memory ran out. */
int  gravity_term_init(struct gravity_term* t, int cols, int rows,
                       int cell_w, int cell_h, unsigned int fg, unsigned int bg);
void gravity_term_free(struct gravity_term* t);

/* Reset the screen and scrollback (like a fresh terminal). */
void gravity_term_clear(struct gravity_term* t);

/* Feed output. '\n' moves down (scrolling at the bottom), '\r' returns to
   column 0, '\b' steps back, '\t' advances to the next 8-column stop, a
   printable byte is stored, and a full row wraps to the next line. */
void gravity_term_putc(struct gravity_term* t, int c);
void gravity_term_write(struct gravity_term* t, const char* buf, int n);

/* Paint the visible grid into 'c' at pixel (ox, oy), drawing the cursor when
   the view is live. Only changed rows are repainted, so the caller must NOT
   clear the canvas between frames — the grid owns every cell. */
void gravity_term_render(struct gravity_term* t, struct gravity_canvas* c,
                         int ox, int oy);

/* Scrollback: 'lines' moves the view up (positive) or down (negative); the
   view clamps to the history. snap_bottom() returns to the live screen. */
void gravity_term_scrollback(struct gravity_term* t, int lines);
void gravity_term_snap_bottom(struct gravity_term* t);

__END_DECLS

#endif

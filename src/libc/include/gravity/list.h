#ifndef _GRAVITY_LIST_H
#define _GRAVITY_LIST_H

#include <gravity/canvas.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

/* A scrollable, single-column list of text rows with a selection. The
   caller owns the item strings and hands them over with set(); the list
   keeps the selection visible and repaints only its own viewport. Row
   height is fixed, so a viewport that is a multiple of it shows whole
   rows. */

struct gravity_list {
    int x, y, w, h;      /* viewport, in canvas pixels */
    int row_h;
    int n;               /* item count */
    int sel;             /* selected index, -1 = none */
    int top;             /* index of the first visible row */
    unsigned int fg, bg, sel_fg, sel_bg;
    const char* const* items;
};

void gravity_list_init(struct gravity_list* l, int x, int y, int w, int h,
                       int row_h, unsigned int fg, unsigned int bg,
                       unsigned int sel_fg, unsigned int sel_bg);

/* Move/resize the viewport, keeping the selection visible. */
void gravity_list_resize(struct gravity_list* l, int x, int y, int w, int h);

/* Replace the items (the caller keeps them alive) and clamp the selection. */
void gravity_list_set(struct gravity_list* l, const char* const* items, int n);

int  gravity_list_rows(const struct gravity_list* l);
/* Item index under a canvas point, or -1 if none / outside the viewport. */
int  gravity_list_hit(const struct gravity_list* l, int px, int py);

void gravity_list_select(struct gravity_list* l, int index); /* reveal it */
void gravity_list_move(struct gravity_list* l, int delta);
void gravity_list_scroll(struct gravity_list* l, int delta);

void gravity_list_draw(struct gravity_list* l, struct gravity_canvas* c);

__END_DECLS

#endif

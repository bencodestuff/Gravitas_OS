#ifndef _GRAVITY_EDITOR_H
#define _GRAVITY_EDITOR_H

#include <gravity/canvas.h>
#include <gravity/buffer.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

/* A text-editing widget: a viewport over a gravity_buffer with a caret, a
   selection, scrolling and the clipboard.

   It owns the buffer and its own geometry, but not the window: the host owns
   the surface, the status line, the title and the save/quit policy. So the
   same widget is a whole app (edit.elf) and one pane of the IDE.

   The region (x, y, w, h) is in canvas pixels, and its last
   GRAVITY_EDITOR_SB columns are the scrollbar strip. Keys and mouse points are
   canvas-absolute, so a host that has already hit-tested can forward them
   unchanged. The widget never clears more than its own region. */

#define GRAVITY_EDITOR_SB 6 /* scrollbar strip on the right, in pixels */
#define GRAVITY_EDITOR_CW 8 /* glyph cell width — the PSF2 console font */

struct gravity_editor {
    struct gravity_buffer text;
    char  path[160];
    int   x, y, w, h;      /* the viewport region, in canvas pixels */
    int   g_ch;            /* glyph height */
    int   rows, cols;      /* derived from the region */
    int   cursor;          /* caret, a byte offset in [0, len] */
    int   sel;             /* selection anchor, -1 = none */
    int   top;             /* byte offset of the first visible line */
    int   left;            /* first visible display column */
    int   goal_col;        /* target column while moving vertically */
    int   modified;
    int   rev;             /* bumped on every edit (a host change counter) */
    int   sb_drag, mouse_held; /* pointer state, so a drag survives leaving */
    char  msg[96];         /* the last action's message, for the host to show */
};

/* Bind the widget to a region and load 'path'. A missing file is an empty
   buffer, not an error. Returns 0, or -1 if the buffer cannot be allocated. */
int  gravity_editor_init(struct gravity_editor* e, const char* path,
                         int x, int y, int w, int h);
void gravity_editor_free(struct gravity_editor* e);

/* Re-read the file, discarding edits / write it back. save() returns the byte
   count written, or -1 (and sets msg). */
void gravity_editor_reload(struct gravity_editor* e);
int  gravity_editor_save(struct gravity_editor* e);

int  gravity_editor_dirty(const struct gravity_editor* e);
int  gravity_editor_len(const struct gravity_editor* e);
int  gravity_editor_rev(const struct gravity_editor* e);
const char* gravity_editor_path(const struct gravity_editor* e);
const char* gravity_editor_basename(const struct gravity_editor* e);
const char* gravity_editor_msg(const struct gravity_editor* e);

/* Replace the message line (for the host's own prompts). */
void gravity_editor_set_msg(struct gravity_editor* e, const char* s);

/* "line:col *" for the host's status line. */
void gravity_editor_status(const struct gravity_editor* e, char* out, int max);

/* An editing key: movement, selection, clipboard, text. Returns 1 if it was
   consumed. Ctrl+S/Q and the host's own chords are deliberately not handled
   here — saving and quitting are policy. */
int  gravity_editor_key(struct gravity_editor* e, long key, int mods);
void gravity_editor_mouse(struct gravity_editor* e, int mx, int my, int held);
void gravity_editor_scroll(struct gravity_editor* e, int lines);

/* Paint the viewport and its scrollbar into 'c'. */
void gravity_editor_draw(struct gravity_editor* e, struct gravity_canvas* c);

__END_DECLS

#endif

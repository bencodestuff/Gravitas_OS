#ifndef _GRAVITY_BUFFER_H
#define _GRAVITY_BUFFER_H

#include <sys/cdefs.h>

__BEGIN_DECLS

/* An editable text buffer: a flat byte array of '\n'-separated lines with
   insert/erase and line navigation. It is pure storage — the cursor, the
   viewport and the drawing belong to the app (see edit.elf).

   Offsets are byte positions in [0, len]; every offset is also a valid
   cursor position, so a line's "end" (the '\n' or len) is where a cursor
   sits after the last character. The buffer grows on demand up to a ceiling
   and never shrinks. */

#define GRAVITY_BUFFER_TAB 4   /* display width of a tab, in cells */

struct gravity_buffer {
    char* buf;
    int   len;
    int   cap;
};

/* Allocate a buffer with room for 'cap' bytes (rounded up to a minimum).
   Returns 0, or -1 on out-of-memory. */
int  gravity_buffer_init(struct gravity_buffer* t, int cap);
void gravity_buffer_free(struct gravity_buffer* t);

/* Replace the whole contents. Returns 0, or -1 if it cannot grow. */
int  gravity_buffer_set(struct gravity_buffer* t, const char* data, int n);

/* Insert 'n' bytes of 's' at 'at' (clamped into range). Returns 0, or -1 if
   it cannot grow. */
int  gravity_buffer_insert(struct gravity_buffer* t, int at, const char* s, int n);

/* Erase 'n' bytes starting at 'at' (clamped). */
void gravity_buffer_erase(struct gravity_buffer* t, int at, int n);

/* Line navigation. 'off' is clamped into [0, len]. */
int  gravity_buffer_line_start(const struct gravity_buffer* t, int off);
int  gravity_buffer_line_end(const struct gravity_buffer* t, int off);
int  gravity_buffer_next_line(const struct gravity_buffer* t, int off);
int  gravity_buffer_prev_line(const struct gravity_buffer* t, int off);
int  gravity_buffer_line_count(const struct gravity_buffer* t);

/* Display column of 'off' within its line, tabs expanded to GRAVITY_BUFFER_TAB
   stops. */
int  gravity_buffer_col(const struct gravity_buffer* t, int off);

/* The offset on the line beginning at 'line_off' closest to display column
   'col' (used for vertical movement and click-to-place). */
int  gravity_buffer_offset_at_col(const struct gravity_buffer* t, int line_off,
                                int col);

__END_DECLS

#endif
#ifndef _GRAVITY_FILER_H
#define _GRAVITY_FILER_H

#include <gravity/canvas.h>
#include <gravity/list.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

/* A directory browser: the current directory, its entries (directories first,
   then case-insensitive by name) and a selection.

   It owns "which files" — the listing, the sort and the navigation — and not
   "how they are shown": the embedded gravity_list is the viewport, so the host
   draws it, hit-tests it and decides what opening an entry means (a directory
   is entered, a file opens in an editor, ...). */

#define GRAVITY_FILER_MAX  160
#define GRAVITY_FILER_NAME 64

struct gravity_filer {
    char cwd[128];                              /* the current directory */
    char listing[2048];                         /* the raw readdir reply */
    char names[GRAVITY_FILER_MAX][GRAVITY_FILER_NAME];
    const char* items[GRAVITY_FILER_MAX];       /* the sorted entry names */
    int  n;
    int  has_dotdot;                            /* ".." is items[0] */
    struct gravity_list list;                   /* the scrolling viewport */
};

/* Bind the browser to a list region and show 'path'. Returns 0 if the directory
   was readable, -1 if not (leaving the current one in place). */
int  gravity_filer_init(struct gravity_filer* f, const char* path,
                        int x, int y, int w, int h, int row_h);

/* Show 'path' (relative to the CWD, or absolute). Returns 0, or -1 if it could
   not be read. */
int  gravity_filer_load(struct gravity_filer* f, const char* path);

/* Move/resize the listing viewport. */
void gravity_filer_resize(struct gravity_filer* f, int x, int y, int w, int h);

/* Go to the parent directory. Returns 0 when already at the root. */
int  gravity_filer_up(struct gravity_filer* f);

/* The selected entry ("name", "dir/" or ".."), or 0 when nothing is selected. */
const char* gravity_filer_selected(const struct gravity_filer* f);

/* The full path of a child of the CWD (a trailing '/' on 'name' is dropped). */
void gravity_filer_path(const struct gravity_filer* f, char* out, int max,
                        const char* name);

/* A listing entry with a trailing '/' is a directory. */
int  gravity_filer_is_dir(const char* name);

/* Enter the selected entry if it is a directory (or ".."). Returns 1 if it did
   — otherwise the host decides what to do with the file. */
int  gravity_filer_enter(struct gravity_filer* f);

__END_DECLS

#endif

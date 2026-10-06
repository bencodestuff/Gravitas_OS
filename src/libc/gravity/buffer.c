#include <gravity/buffer.h>
#include <stdlib.h>

/* See gravity/buffer.h. Storage only: insert/erase move bytes with a simple
   shift, which is plenty for the file sizes an editor on this system sees.
   Growth doubles from a floor up to a ceiling so a runaway paste cannot eat
   the whole heap. */

#define TEXT_MIN_CAP 256
#define TEXT_MAX_CAP (4 << 20) /* 4 MB ceiling */

int gravity_buffer_init(struct gravity_buffer* t, int cap) {
    if (cap < TEXT_MIN_CAP) cap = TEXT_MIN_CAP;
    t->buf = (char*)malloc((unsigned long)cap);
    t->len = 0;
    t->cap = t->buf ? cap : 0;
    return t->buf ? 0 : -1;
}

void gravity_buffer_free(struct gravity_buffer* t) {
    free(t->buf);
    t->buf = 0;
    t->len = 0;
    t->cap = 0;
}

static int grow(struct gravity_buffer* t, int need) {
    if (need <= t->cap) return 0;
    if (need > TEXT_MAX_CAP) return -1;
    long cap = t->cap ? t->cap : TEXT_MIN_CAP;
    while (cap < need) cap *= 2;
    if (cap > TEXT_MAX_CAP) cap = TEXT_MAX_CAP;
    char* nb = (char*)realloc(t->buf, (unsigned long)cap);
    if (!nb) return -1;
    t->buf = nb;
    t->cap = (int)cap;
    return 0;
}

int gravity_buffer_set(struct gravity_buffer* t, const char* data, int n) {
    if (n < 0) n = 0;
    if (grow(t, n) != 0) return -1;
    for (int i = 0; i < n; i++) t->buf[i] = data[i];
    t->len = n;
    return 0;
}

int gravity_buffer_insert(struct gravity_buffer* t, int at, const char* s, int n) {
    if (n <= 0) return 0;
    if (at < 0) at = 0;
    if (at > t->len) at = t->len;
    if (grow(t, t->len + n) != 0) return -1;
    for (int i = t->len - 1; i >= at; i--) t->buf[i + n] = t->buf[i];
    for (int i = 0; i < n; i++) t->buf[at + i] = s[i];
    t->len += n;
    return 0;
}

void gravity_buffer_erase(struct gravity_buffer* t, int at, int n) {
    if (at < 0 || n <= 0 || at >= t->len) return;
    if (at + n > t->len) n = t->len - at;
    for (int i = at; i + n < t->len; i++) t->buf[i] = t->buf[i + n];
    t->len -= n;
}

int gravity_buffer_line_start(const struct gravity_buffer* t, int off) {
    if (off > t->len) off = t->len;
    if (off < 0) off = 0;
    while (off > 0 && t->buf[off - 1] != '\n') off--;
    return off;
}

int gravity_buffer_line_end(const struct gravity_buffer* t, int off) {
    if (off < 0) off = 0;
    if (off > t->len) off = t->len;
    while (off < t->len && t->buf[off] != '\n') off++;
    return off;
}

int gravity_buffer_next_line(const struct gravity_buffer* t, int off) {
    int e = gravity_buffer_line_end(t, off);
    return e < t->len ? e + 1 : t->len;
}

int gravity_buffer_prev_line(const struct gravity_buffer* t, int off) {
    int s = gravity_buffer_line_start(t, off);
    return s > 0 ? gravity_buffer_line_start(t, s - 1) : 0;
}

int gravity_buffer_line_count(const struct gravity_buffer* t) {
    int n = 1;
    for (int i = 0; i < t->len; i++)
        if (t->buf[i] == '\n') n++;
    return n;
}

static int char_width(int col, char c) {
    if (c != '\t') return 1;
    return GRAVITY_BUFFER_TAB - (col % GRAVITY_BUFFER_TAB);
}

int gravity_buffer_col(const struct gravity_buffer* t, int off) {
    int s = gravity_buffer_line_start(t, off);
    int col = 0;
    for (int i = s; i < off; i++) col += char_width(col, t->buf[i]);
    return col;
}

int gravity_buffer_offset_at_col(const struct gravity_buffer* t, int line_off,
                               int col) {
    int i = gravity_buffer_line_start(t, line_off);
    int c = 0;
    while (i < t->len && t->buf[i] != '\n') {
        int w = char_width(c, t->buf[i]);
        if (c + w > col) break; // the target column is inside this cell
        c += w;
        i++;
    }
    return i;
}
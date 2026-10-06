#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <grv/syscall.h>

// The kernel's readdir fills one newline-separated listing, so opendir
// snapshots it and readdir walks the lines. d_type comes from the trailing '/'
// the listing uses for directories; there are no inode numbers.

#define LIST_MAX 8192

struct __dirstream {
    char* buf;
    size_t len;
    size_t pos;
    struct dirent ent;
};

DIR* opendir(const char* name) {
    if (name == 0) return 0;

    struct grv_stat st;
    if (grv_stat(name, &st) != 0 || !st.is_dir) return 0;   // errno is set

    char* buf = (char*)malloc(LIST_MAX);
    if (buf == 0) return 0;
    grv_readdir(name, buf, LIST_MAX);
    buf[LIST_MAX - 1] = '\0';

    DIR* d = (DIR*)malloc(sizeof(DIR));
    if (d == 0) { free(buf); return 0; }
    d->buf = buf;
    d->len = strlen(buf);
    d->pos = 0;
    return d;
}

struct dirent* readdir(DIR* d) {
    if (d == 0 || d->pos >= d->len) return 0;

    size_t start = d->pos;
    size_t i = start;
    while (i < d->len && d->buf[i] != '\n') i++;

    size_t n = i - start;
    int is_dir = (n > 0 && d->buf[start + n - 1] == '/');
    if (is_dir) n--;
    if (n >= sizeof(d->ent.d_name)) n = sizeof(d->ent.d_name) - 1;

    memcpy(d->ent.d_name, d->buf + start, n);
    d->ent.d_name[n] = '\0';
    d->ent.d_ino = 0;
    d->ent.d_type = is_dir ? DT_DIR : DT_REG;

    d->pos = (i < d->len) ? i + 1 : i;
    return &d->ent;
}

int closedir(DIR* d) {
    if (d == 0) return -1;
    free(d->buf);
    free(d);
    return 0;
}

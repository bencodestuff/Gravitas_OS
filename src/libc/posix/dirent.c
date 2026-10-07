#include <dirent.h>
#include <stdlib.h>
#include <grv/syscall.h>

// opendir reads the directory into an array of the kernel's typed entries;
// readdir walks that array. The inode number and the type come straight from
// the kernel, so d_ino and d_type mean something.
//
// The array grows: the kernel fills at most what it is given, so a full reply
// means "there may be more" and the buffer doubles. Any size of directory is
// therefore readable, and the common case costs one call and one small buffer.

#define DIR_START 32

struct __dirstream {
    struct grv_dirent* ents;
    long n;
    long pos;
    struct dirent ent;
};

DIR* opendir(const char* name) {
    if (name == 0) return 0;

    struct grv_stat st;
    if (grv_stat(name, &st) != 0 || !st.is_dir) return 0;   // errno is set

    long cap = DIR_START;
    struct grv_dirent* ents = 0;
    long n = 0;
    for (;;) {
        struct grv_dirent* grown =
            (struct grv_dirent*)realloc(ents, (size_t)cap * sizeof(*ents));
        if (grown == 0) { free(ents); return 0; }
        ents = grown;

        n = grv_readdir(name, ents, cap);
        if (n < 0) { free(ents); return 0; }
        if (n < cap) break;              // the whole directory fit
        cap *= 2;
    }

    DIR* d = (DIR*)malloc(sizeof(DIR));
    if (d == 0) { free(ents); return 0; }
    d->ents = ents;
    d->n = n;
    d->pos = 0;
    return d;
}

struct dirent* readdir(DIR* d) {
    if (d == 0 || d->pos >= d->n) return 0;
    const struct grv_dirent* e = &d->ents[d->pos++];

    size_t i = 0;
    while (i + 1 < sizeof(d->ent.d_name) && e->name[i]) {
        d->ent.d_name[i] = e->name[i];
        i++;
    }
    d->ent.d_name[i] = '\0';
    d->ent.d_ino = (ino_t)e->ino;
    d->ent.d_type = e->type;
    return &d->ent;
}

int closedir(DIR* d) {
    if (d == 0) return -1;
    free(d->ents);
    free(d);
    return 0;
}

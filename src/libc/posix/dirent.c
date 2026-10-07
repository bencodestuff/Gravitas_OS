#include <dirent.h>
#include <stdlib.h>
#include <grv/syscall.h>

// opendir snapshots the directory into an array of the kernel's typed entries;
// readdir walks that array. The inode number and the type come straight from
// the kernel now, so d_ino and d_type mean something.

#define DIR_MAX 256

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

    struct grv_dirent* ents =
        (struct grv_dirent*)malloc(DIR_MAX * sizeof(*ents));
    if (ents == 0) return 0;

    long n = grv_readdir(name, ents, DIR_MAX);
    if (n < 0) { free(ents); return 0; }

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

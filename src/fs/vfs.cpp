#include "fs/vfs.h"
#include "kernel/gfx/framebuffer.h"
#include "kernel/core/heap.h"
#include "kernel/core/sched.h"
#include "kernel/core/errno.h"

extern "C" void kprint(const char* str);
extern "C" void kprint_uint(uint64_t value);

namespace VFS {

static Mount mounts[MAX_MOUNTS];
static int mount_count = 0;
static FileHandle handles[MAX_HANDLES];

void init() {
    for (int i = 0; i < MAX_MOUNTS; i++) mounts[i].path = nullptr;
    for (int i = 0; i < MAX_HANDLES; i++) handles[i].node = nullptr;
}

// --- Path normalization ---
// Canonicalizes a path into 'out': absolute, no trailing slash, resolves
// '.' and '..'. Returns false when the path cannot be represented at all (more
// components than can be tracked, or too long for 'out'). Dropping a component
// silently would name a *different* file, which is the one failure mode worth
// being loud about.
static bool normalize(const char* path, char* out, uint64_t out_max) {
    // component starts, as offsets into out — on the stack, like every other
    // buffer on this path: a syscall can be preempted by the timer, so a static
    // would be shared with whatever task runs next.
    uint64_t starts[32];
    int depth = 0;
    uint64_t len = 0;

    out[0] = '\0';
    if (path == nullptr) return false;

    const char* p = path;
    while (*p) {
        while (*p == '/') p++;              // collapse slashes
        if (*p == '\0') break;

        // component = [p, end)
        const char* end = p;
        while (*end && *end != '/') end++;
        uint64_t clen = (uint64_t)(end - p);

        if (clen == 1 && p[0] == '.') {
            // skip
        } else if (clen == 2 && p[0] == '.' && p[1] == '.') {
            if (depth > 0) { len = starts[--depth]; out[len] = '\0'; }
        } else {
            if (depth >= (int)(sizeof(starts) / sizeof(starts[0]))) return false;
            if (len + clen + 2 >= out_max) return false;
            starts[depth++] = len;
            out[len++] = '/';
            for (uint64_t i = 0; i < clen; i++) out[len++] = p[i];
            out[len] = '\0';
        }
        p = end;
    }

    if (len == 0) { out[0] = '/'; out[1] = '\0'; }
    return true;
}

// --- Mount resolution: longest matching prefix wins ---
static Mount* find_mount(const char* canon_path) {
    Mount* best = nullptr;
    uint64_t best_len = 0;
    for (int i = 0; i < mount_count; i++) {
        const char* mp = mounts[i].path;
        uint64_t ml = 0;
        while (mp[ml]) ml++;
        bool matches = true;
        for (uint64_t j = 0; j < ml; j++) {
            if (canon_path[j] != mp[j]) { matches = false; break; }
        }
        // prefix must end at a boundary: "/bin" matches "/bin", "/bin/x",
        // but not "/binary". Exception: the root mount "/" matches every
        // absolute path (its prefix IS the leading slash).
        if (ml == 1 && mp[0] == '/') {
            matches = true;
        } else if (matches && canon_path[ml] != '/' && canon_path[ml] != '\0') {
            matches = false;
        }
        if (matches && ml >= best_len) {
            best = &mounts[i];
            best_len = ml;
        }
    }
    return best;
}

// Resolve a path to a filesystem, writing the fs-relative path into 'rel_out'
// (the caller owns that storage). Longest mount-prefix wins. Nothing here is
// shared between calls, so two resolutions can be live at once — rename()
// needs exactly that. Returns nullptr if nothing covers the path, setting *err
// to why: ENAMETOOLONG when the path cannot be represented at all, ENOENT when
// no mount covers it.
static Filesystem* resolve(const char* path, char* rel_out, uint64_t rel_max,
                           int* err) {
    char canon[PATH_MAX];
    if (!normalize(path, canon, sizeof(canon))) {
        rel_out[0] = '\0';
        if (err) *err = (int)Errno::ENAMETOOLONG;
        return nullptr;
    }

    Mount* m = find_mount(canon);
    if (m == nullptr) {
        rel_out[0] = '\0';
        if (err) *err = (int)Errno::ENOENT;
        return nullptr;
    }

    uint64_t ml = 0;
    while (m->path[ml]) ml++;
    const char* rel = canon + ml;          // "/bin/ls.elf" -> "/ls.elf"
    if (*rel == '/') rel++;                // -> "ls.elf"; "/" -> ""

    uint64_t i = 0;
    while (rel[i]) {
        if (i + 1 >= rel_max) {            // as in normalize: never truncate
            rel_out[0] = '\0';
            if (err) *err = (int)Errno::ENAMETOOLONG;
            return nullptr;
        }
        rel_out[i] = rel[i];
        i++;
    }
    rel_out[i] = '\0';

    // A component longer than NAME_MAX cannot name anything, and saying so here
    // means every caller reports ENAMETOOLONG instead of a lookup that failed
    // for reasons the caller cannot see. (The walkers refuse it too; this is the
    // one that can say why.)
    for (const char* c = rel_out; *c; ) {
        uint64_t len = 0;
        while (c[len] && c[len] != '/') len++;
        if (len >= (uint64_t)NAME_MAX) {
            rel_out[0] = '\0';
            if (err) *err = (int)Errno::ENAMETOOLONG;
            return nullptr;
        }
        c += len;
        while (*c == '/') c++;
    }
    return m->fs;
}

// --- CWD-aware path building ---
// Relative paths (no leading '/') resolve against the current task's CWD.
static uint64_t strlen_(const char* s) {
    uint64_t n = 0;
    while (s[n]) n++;
    return n;
}

static char* effective_path(const char* path, char* out, uint64_t out_max) {
    if (path[0] == '/') {
        uint64_t i = 0;
        for (; path[i] && i + 1 < out_max; i++) out[i] = path[i];
        out[i] = '\0';
        return out;
    }

    // Relative: CWD + '/' + path (normalize() cleans it up afterwards)
    const char* cwd = Sched::current_task_cwd();
    uint64_t i = 0;
    for (; cwd[i] && i + 1 < out_max; i++) out[i] = cwd[i];
    if (i + 1 < out_max) out[i++] = '/';
    for (; *path && i + 1 < out_max; i++) out[i] = *path++;
    out[i] = '\0';
    return out;
}

// --- Bare-name fallback (the program search path) ---
// A name with no slash ("ls.elf") is looked up in the CWD first and then in
// each program directory in order. Programs live in /bin; the test suite
// ships under /bin/tests. Returns the first that exists, else a /bin path
// (so error messages name what was actually tried).
static const char* const kProgramPath[] = { "/bin", "/bin/tests" };

// Does the absolute path 'full' name an existing file or directory?
static bool path_exists(const char* full) {
    char rel[PATH_MAX];
    Filesystem* fs = resolve(full, rel, sizeof(rel), nullptr);
    if (fs == nullptr) return false;
    Node* n = node_walk(fs, rel);
    if (n == nullptr) return false;
    fs->release(fs->priv, n);
    return true;
}

// "<dir>/<name>" into out.
static char* join_dir(const char* dir, const char* name, char* out, uint64_t out_max) {
    uint64_t i = 0;
    for (; *dir && i + 1 < out_max; dir++) out[i++] = *dir;
    if (i + 1 < out_max) out[i++] = '/';
    for (; *name && i + 1 < out_max; name++) out[i++] = *name;
    out[i] = '\0';
    return out;
}

static bool resolve_path_fallback(const char* path, char* out, uint64_t out_max,
                                  const char** effective, bool for_create) {
    bool has_slash = false;
    for (const char* p = path; *p; p++) {
        if (*p == '/') { has_slash = true; break; }
    }

    // "." and ".." are pure navigation — never treat them as program names
    if (has_slash || path[0] == '\0' ||
        (path[0] == '.' && (path[1] == '\0' || (path[1] == '.' && path[2] == '\0')))) {
        *effective = effective_path(path, out, out_max);
        return true;
    }

    // try CWD-relative: if it exists there (file OR dir), use it
    char cwdtry[PATH_MAX];
    effective_path(path, cwdtry, sizeof(cwdtry));
    if (path_exists(cwdtry)) {
        *effective = effective_path(path, out, out_max);
        return true;
    }

    // A creation never searches: a bare name is then purely CWD-relative, the
    // same rule mkdir and unlink follow. Otherwise `open("notes.txt", O_CREATE)`
    // would create /bin/notes.txt just because nothing by that name exists yet.
    if (for_create) {
        *effective = effective_path(path, out, out_max);
        return true;
    }

    // then each program directory in turn
    for (uint64_t i = 0; i < sizeof(kProgramPath) / sizeof(kProgramPath[0]); i++) {
        join_dir(kProgramPath[i], path, out, out_max);
        if (path_exists(out)) { *effective = out; return true; }
    }

    // nothing found: report the first search directory
    join_dir(kProgramPath[0], path, out, out_max);
    *effective = out;
    return true;
}

// --- Nodes: the one place that walks a path ---

Node* node_new(Filesystem* fs, FileType type) {
    Node* n = new Node;
    if (n == nullptr) return nullptr;
    n->fs = fs;
    n->priv = nullptr;
    n->type = type;
    n->size = 0;
    n->ino = 0;
    n->mode = 0;
    n->mtime = 0;
    return n;
}

// One component of a path into 'out' (NAME_MAX bytes); advances *p. A component
// that does not fit is an error, not something to truncate: a truncated name
// could match a different file.
enum CompResult { COMP_END, COMP_OK, COMP_TOO_LONG };

static CompResult next_component(const char** p, char* out) {
    const char* s = *p;
    while (*s == '/') s++;
    if (*s == '\0') { *p = s; return COMP_END; }

    uint64_t i = 0;
    while (*s && *s != '/') {
        if (i + 1 >= NAME_MAX) return COMP_TOO_LONG;
        out[i++] = *s++;
    }
    out[i] = '\0';
    *p = s;
    return COMP_OK;
}

Node* node_walk(Filesystem* fs, const char* rel) {
    if (fs == nullptr || fs->root == nullptr) return nullptr;
    Node* cur = fs->root(fs->priv);
    if (cur == nullptr) return nullptr;
    if (rel == nullptr || rel[0] == '\0') return cur;

    char comp[NAME_MAX];
    const char* p = rel;
    CompResult r;
    while ((r = next_component(&p, comp)) == COMP_OK) {
        if (cur->type != FT_DIR) { fs->release(fs->priv, cur); return nullptr; }
        Node* next = fs->lookup(fs->priv, cur, comp);
        fs->release(fs->priv, cur);
        if (next == nullptr) return nullptr;
        cur = next;
    }
    if (r == COMP_TOO_LONG) { fs->release(fs->priv, cur); return nullptr; }
    return cur;
}

Node* node_create(Filesystem* fs, const char* rel, FileType final_type) {
    if (fs == nullptr || fs->root == nullptr || fs->create == nullptr)
        return nullptr;
    Node* cur = fs->root(fs->priv);
    if (cur == nullptr) return nullptr;
    if (rel == nullptr || rel[0] == '\0') return cur;

    char comp[NAME_MAX];
    const char* p = rel;
    CompResult r;
    while ((r = next_component(&p, comp)) == COMP_OK) {
        bool last = true;
        for (const char* q = p; *q; q++)
            if (*q != '/') { last = false; break; }

        Node* next = fs->lookup(fs->priv, cur, comp);
        if (next == nullptr) {
            Node* made = nullptr;
            if (fs->create(fs->priv, cur, comp, last ? final_type : FT_DIR,
                           &made) != 0) {
                fs->release(fs->priv, cur);
                return nullptr;
            }
            next = made;
        }
        fs->release(fs->priv, cur);
        cur = next;
    }
    if (r == COMP_TOO_LONG) { fs->release(fs->priv, cur); return nullptr; }
    return cur;
}

bool node_split(const char* rel, char* parent, char* name) {
    uint64_t len = strlen_(rel);
    while (len > 0 && rel[len - 1] == '/') len--;

    uint64_t slash = 0;                    // position just past the last '/'
    for (uint64_t i = 0; i < len; i++)
        if (rel[i] == '/') slash = i + 1;

    uint64_t n = 0;
    for (uint64_t i = slash; i < len; i++) {
        if (n + 1 >= NAME_MAX) return false;   // a name too long to be one
        name[n++] = rel[i];
    }
    name[n] = '\0';
    if (n == 0) return false;

    uint64_t plen = (slash > 0) ? slash - 1 : 0;   // drop the trailing '/'
    for (uint64_t i = 0; i < plen; i++) parent[i] = rel[i];
    parent[plen] = '\0';
    return true;
}

bool mount(const char* path, Filesystem* fs) {
    if (mount_count >= MAX_MOUNTS) return false;
    static char canon_storage[MAX_MOUNTS][128];
    if (!normalize(path, canon_storage[mount_count], 128)) return false;
    mounts[mount_count].path = canon_storage[mount_count];
    mounts[mount_count].fs = fs;
    mount_count++;

    // Create the mountpoint directory in the parent filesystem, so `ls /`
    // shows it (e.g. /bin appearing in the ramfs root). The parent mount
    // must already be registered (mount "/" first).
    const char* mp = mounts[mount_count-1].path;
    if (mp[1] != '\0') { // skip "/" itself
        // find the longest OTHER mount that covers this path
        Mount* parent = nullptr;
        uint64_t best_len = 0;
        for (int i = 0; i < mount_count - 1; i++) {
            const char* pp = mounts[i].path;
            uint64_t pl = strlen_(pp);
            // same rule as find_mount: root "/" covers everything;
            // otherwise the char right after the prefix must end the
            // parent component ('/' boundary or end-of-string)
            bool covers = (pl == 1 && pp[0] == '/');
            if (!covers) {
                covers = true;
                for (uint64_t j = 0; j < pl; j++) {
                    if (mp[j] != pp[j]) { covers = false; break; }
                }
                if (covers && pl < strlen_(mp) && mp[pl] != '/') covers = false;
            }
            if (covers && pl > best_len) {
                parent = &mounts[i];
                best_len = pl;
            }
        }
        if (parent) {
            // strip the parent prefix -> fs-relative path (root's rel
            // paths have no leading slash; deeper mounts: skip the '/')
            const char* rel = mp + best_len;
            if (best_len > 1 && *rel == '/') rel++;
            Node* made = node_create(parent->fs, rel, FT_DIR);
            if (made) parent->fs->release(parent->fs->priv, made);
        }
    }
    return true;
}

bool is_directory(const char* path) {
    char fixed[PATH_MAX];
    const char* effective = nullptr;
    resolve_path_fallback(path, fixed, sizeof(fixed), &effective, false);

    char rel[PATH_MAX];
    Filesystem* fs = resolve(effective, rel, sizeof(rel), nullptr);
    if (fs == nullptr) return false;
    Node* n = node_walk(fs, rel);
    if (n == nullptr) return false;
    bool dir = (n->type == FT_DIR);
    fs->release(fs->priv, n);
    return dir;
}

bool canonicalize(const char* path, char* out, uint64_t out_max) {
    // Go through the same fallback resolution is_directory uses —
    // otherwise `cd Pictures` validates against /bin/Pictures but STORES
    // /Pictures (a nonexistent path), and every later ls/cd breaks.
    char fixed[PATH_MAX];
    const char* effective = nullptr;
    resolve_path_fallback(path, fixed, sizeof(fixed), &effective, false);
    return normalize(effective, out, out_max);
}

// --- fd plumbing: the table lives in the current task ---
void fds_init_current() {
    FileHandle** fds = Sched::current_task_fds();
    for (int i = 0; i < MAX_FDS; i++) fds[i] = nullptr;
}

FileHandle** fds_of_current() {
    return Sched::current_task_fds();
}

FileHandle* fd_slot_get(int fd) {
    if (fd < 0 || fd >= MAX_FDS) return nullptr;
    return fds_of_current()[fd];
}

int fd_slot_alloc(Node* node) {
    FileHandle** fds = fds_of_current();
    // Start at FD_STD_STREAMS: slots 0..2 are reserved for the standard
    // streams, so the first descriptor a program opens is 3.
    for (int i = FD_STD_STREAMS; i < MAX_FDS; i++) {
        if (fds[i] == nullptr) {
            // grab a kernel-wide handle slot
            FileHandle* h = nullptr;
            for (int j = 0; j < MAX_HANDLES; j++) {
                if (handles[j].node == nullptr) { h = &handles[j]; break; }
            }
            if (h == nullptr) return -1; // out of global handles

            h->node = node;
            h->offset = 0;
            fds[i] = h;
            return i;
        }
    }
    return -1; // fd table full
}

// --- fd operations ---
// Failures return a negated Errno code (see kernel/core/errno.h); libc turns
// them into -1 + errno.
int open_fd(const char* path, int flags) {
    char fixed[PATH_MAX];
    const char* effective = nullptr;
    // A creation is CWD-relative: it must not search /bin (see
    // resolve_path_fallback).
    resolve_path_fallback(path, fixed, sizeof(fixed), &effective,
                          (flags & O_CREATE) != 0);

    char rel[PATH_MAX];
    int err = 0;
    Filesystem* fs = resolve(effective, rel, sizeof(rel), &err);
    if (fs == nullptr) return -err;

    // A filesystem with no write at all is read-only, so a request that would
    // need one is refused here rather than handed an fd that cannot deliver.
    if ((flags & (O_WRONLY | O_RDWR | O_CREATE | O_TRUNC)) && fs->write == nullptr)
        return -Errno::EROFS;

    Node* node = node_walk(fs, rel);
    if (node == nullptr) {
        if (!(flags & O_CREATE)) return -Errno::ENOENT;
        if (fs->create == nullptr) return -Errno::EROFS;
        node = node_create(fs, rel, FT_FILE);
        if (node == nullptr) return -Errno::EIO;
    }
    if (node->type == FT_DIR) {
        fs->release(fs->priv, node);
        return -Errno::EISDIR;
    }

    if (flags & O_TRUNC) {
        if (fs->truncate == nullptr) {
            fs->release(fs->priv, node);
            return -Errno::EROFS;
        }
        if (fs->truncate(fs->priv, node, 0) != 0) {
            fs->release(fs->priv, node);
            return -Errno::EIO;
        }
        node->size = 0;
    }

    int fd = fd_slot_alloc(node);
    if (fd == -1) {
        fs->release(fs->priv, node); // don't leak the node
        return -Errno::EMFILE;
    }
    return fd;
}

long read_fd(int fd, void* buf, uint64_t count) {
    FileHandle* h = fd_slot_get(fd);
    if (h == nullptr) return -Errno::EBADF;
    Node* n = h->node;
    if (n->fs->read == nullptr) return -Errno::EACCES;
    // Clamp against the node's size (protects zero-copy backends like tarfs
    // whose read cannot know its own bounds, and is live for a file another
    // fd may have extended).
    if (h->offset >= n->size) return 0;
    if (count > n->size - h->offset) count = n->size - h->offset;
    uint64_t got = n->fs->read(n->fs->priv, n, buf, count, h->offset);
    h->offset += got;
    return (long)got;
}

long write_fd(int fd, const void* buf, uint64_t count) {
    FileHandle* h = fd_slot_get(fd);
    if (h == nullptr) return -Errno::EBADF;
    Node* n = h->node;
    if (n->fs->write == nullptr) return -Errno::EROFS;
    uint64_t got = n->fs->write(n->fs->priv, n, buf, count, h->offset);
    // A backend that writes nothing for a real request has run out of room
    // (a read-only backend never gets here: it has no write at all).
    if (got == 0 && count > 0) return -Errno::ENOSPC;
    h->offset += got;
    // The size lives on the node, so every fd (and a later lseek END) sees a
    // file that grew, which is what an inode means.
    if (h->offset > n->size) n->size = h->offset;
    return (long)got;
}

void close_fd_for(FileHandle** fds, int fd) {
    if (fd < 0 || fd >= MAX_FDS) return;
    FileHandle* h = fds[fd];
    if (h == nullptr) return;
    if (h->node) h->node->fs->release(h->node->fs->priv, h->node);
    h->node = nullptr;   // release the global handle slot
    fds[fd] = nullptr;
}

void close_fd(int fd) {
    close_fd_for(fds_of_current(), fd);
}

long lseek_fd(int fd, long offset, int whence) {
    FileHandle* h = fd_slot_get(fd);
    if (h == nullptr) return -Errno::EBADF;
    long base;
    if (whence == 0) base = 0;                 // SET
    else if (whence == 1) base = (long)h->offset; // CUR
    else if (whence == 2) base = (long)h->node->size; // END
    else return -Errno::EINVAL;

    long pos = base + offset;
    if (pos < 0) pos = 0;
    if ((uint64_t)pos > h->node->size) pos = (long)h->node->size; // not past EOF
    h->offset = (uint64_t)pos;
    return pos;
}

// Path rule (see vfs.h): lookups (open/stat) accept a bare name and search
// the CWD then /bin; creation and removal (open with O_CREATE, mkdir, unlink)
// are always CWD-relative, so a typo never lands in a program directory.
int mkdir(const char* path) {
    char rel[PATH_MAX];
    int err = 0;
    Filesystem* fs = resolve(path, rel, sizeof(rel), &err);
    if (fs == nullptr) return -err;
    if (fs->create == nullptr) return -Errno::EROFS;

    // The parent has to exist. `mkdir a/b` must not invent `a`: a typo would
    // create a tree, and a tree is not what one directory means. (`-p` is the
    // userland loop over the prefixes that POSIX says it is.)
    char parent[PATH_MAX], name[NAME_MAX];
    if (!node_split(rel, parent, name)) return -Errno::EINVAL;
    Node* dir = node_walk(fs, parent);
    if (dir == nullptr) return -Errno::ENOENT;
    if (dir->type != FT_DIR) { fs->release(fs->priv, dir); return -Errno::ENOTDIR; }

    Node* made = nullptr;
    int r = fs->create(fs->priv, dir, name, FT_DIR, &made);
    fs->release(fs->priv, dir);
    if (r != 0) return -Errno::EEXIST;
    fs->release(fs->priv, made);
    return 0;
}

int unlink(const char* path) {
    char rel[PATH_MAX];
    int err = 0;
    Filesystem* fs = resolve(path, rel, sizeof(rel), &err);
    if (fs == nullptr) return -err;
    if (fs->unlink == nullptr) return -Errno::EROFS;

    char parent[PATH_MAX], name[NAME_MAX];
    if (!node_split(rel, parent, name)) return -Errno::ENAMETOOLONG;
    Node* dir = node_walk(fs, parent);
    if (dir == nullptr) return -Errno::ENOENT;
    int r = fs->unlink(fs->priv, dir, name);
    fs->release(fs->priv, dir);
    return r == 0 ? 0 : -Errno::ENOENT;
}

// Rename resolves like mkdir/unlink: against the CWD (or an absolute path)
// only, never through the program search path. Both sides must be on the same
// filesystem — there is no cross-device move.
int rename(const char* from, const char* to) {
    char from_rel[PATH_MAX], to_rel[PATH_MAX];
    int err = 0;

    Filesystem* from_fs = resolve(from, from_rel, sizeof(from_rel), &err);
    if (from_fs == nullptr) return -err;

    Filesystem* to_fs = resolve(to, to_rel, sizeof(to_rel), &err);
    if (to_fs == nullptr) return -err;

    if (from_fs != to_fs) return -Errno::EINVAL;
    if (from_fs->rename == nullptr) return -Errno::EROFS;

    char from_parent[PATH_MAX], from_name[NAME_MAX];
    char to_parent[PATH_MAX], to_name[NAME_MAX];
    if (!node_split(from_rel, from_parent, from_name)) return -Errno::ENAMETOOLONG;
    if (!node_split(to_rel, to_parent, to_name)) return -Errno::ENAMETOOLONG;

    Node* fdir = node_walk(from_fs, from_parent);
    if (fdir == nullptr) return -Errno::ENOENT;
    Node* tdir = node_walk(from_fs, to_parent);
    if (tdir == nullptr) { from_fs->release(from_fs->priv, fdir); return -Errno::ENOENT; }

    int r = from_fs->rename(from_fs->priv, fdir, from_name, tdir, to_name);
    from_fs->release(from_fs->priv, fdir);
    from_fs->release(from_fs->priv, tdir);
    return r == 0 ? 0 : -Errno::ENOENT;
}

int ftruncate_fd(int fd, uint64_t size) {
    FileHandle* h = fd_slot_get(fd);
    if (h == nullptr) return -Errno::EBADF;
    Node* n = h->node;
    if (n->fs->truncate == nullptr) return -Errno::EROFS;
    int r = n->fs->truncate(n->fs->priv, n, size);
    if (r != 0) return -Errno::ENOSPC; // the only reason a truncate fails here
    n->size = size;
    return 0;
}

int stat_path(const char* path, Stat* out) {
    // A lookup: same resolution as open (CWD, then /bin for a bare name).
    char fixed[PATH_MAX];
    const char* effective = nullptr;
    resolve_path_fallback(path, fixed, sizeof(fixed), &effective, false);

    char rel[PATH_MAX];
    int err = 0;
    Filesystem* fs = resolve(effective, rel, sizeof(rel), &err);
    if (fs == nullptr) return -err;

    Node* n = node_walk(fs, rel);
    if (n == nullptr) return -Errno::ENOENT;
    if (out) {
        out->size = n->size;
        out->type = n->type;
        out->ino = n->ino;
        out->mode = n->mode;
        out->mtime = n->mtime;
    }
    fs->release(fs->priv, n);
    return 0;
}

// --- How much room the filesystem holding a path has ---
int statfs_path(const char* path, StatFs* out) {
    if (out == nullptr) return -Errno::EINVAL;

    char fixed[PATH_MAX];
    const char* effective = nullptr;
    resolve_path_fallback(path, fixed, sizeof(fixed), &effective, false);

    char rel[PATH_MAX];
    int err = 0;
    Filesystem* fs = resolve(effective, rel, sizeof(rel), &err);
    if (fs == nullptr) return -err;

    // The path has to name something, as it does for stat: answering with the
    // filesystem a *missing* path would have lived on is worse than saying so.
    Node* n = node_walk(fs, rel);
    if (n == nullptr) return -Errno::ENOENT;
    fs->release(fs->priv, n);

    out->total = 0;
    out->free = 0;
    out->writable = (fs->write != nullptr);
    out->type = fs->fs_type;

    // A backend that can say more, says it. One that cannot leaves the defaults
    // above, which are honest: unknown total, nothing free unless it can write.
    if (fs->statfs != nullptr) fs->statfs(fs->priv, out);
    return 0;
}

// --- A file's bytes, for a zero-copy mapping (see vfs.h) ---
int map_file(const char* path, uint64_t* out_phys, uint64_t* out_size) {
    char fixed[PATH_MAX];
    const char* effective = nullptr;
    resolve_path_fallback(path, fixed, sizeof(fixed), &effective, false);

    char rel[PATH_MAX];
    int err = 0;
    Filesystem* fs = resolve(effective, rel, sizeof(rel), &err);
    if (fs == nullptr) return -err;
    if (fs->map == nullptr) return -Errno::ENOENT;

    Node* n = node_walk(fs, rel);
    if (n == nullptr) return -Errno::ENOENT;
    int r = fs->map(fs->priv, n, out_phys, out_size);
    fs->release(fs->priv, n);
    return r == 0 ? 0 : -Errno::ENOENT;
}

// --- Whole-file read (spawn + kernel use) ---
bool read_full(const char* path, char* buf, uint64_t max, uint64_t* out_size) {
    char fixed[PATH_MAX];
    const char* effective = nullptr;
    resolve_path_fallback(path, fixed, sizeof(fixed), &effective, false);

    char rel[PATH_MAX];
    Filesystem* fs = resolve(effective, rel, sizeof(rel), nullptr);
    if (fs == nullptr || fs->read == nullptr) return false;

    Node* n = node_walk(fs, rel);
    if (n == nullptr || n->type != FT_FILE) {
        if (n) fs->release(fs->priv, n);
        return false;
    }

    uint64_t want = (n->size < max) ? n->size : max;
    uint64_t got = fs->read(fs->priv, n, buf, want, 0);
    fs->release(fs->priv, n);
    if (out_size) *out_size = got;
    return true;
}

// --- directory entries, typed (the LISTDIR syscall) ---
long readdir_entries(const char* path, DirEntry* out, uint64_t max) {
    char fixed[PATH_MAX];
    const char* effective = nullptr;
    resolve_path_fallback(path, fixed, sizeof(fixed), &effective, false);

    char rel[PATH_MAX];
    int err = 0;
    Filesystem* fs = resolve(effective, rel, sizeof(rel), &err);
    if (fs == nullptr) return -err;
    if (fs->readdir == nullptr) return -Errno::ENOENT;

    Node* dir = node_walk(fs, rel);
    if (dir == nullptr) return -Errno::ENOENT;
    if (dir->type != FT_DIR) { fs->release(fs->priv, dir); return -Errno::ENOTDIR; }

    uint64_t n = 0;
    while (n < max && fs->readdir(fs->priv, dir, n, &out[n])) n++;
    fs->release(fs->priv, dir);
    return (long)n;
}

// --- fd size accessor (spawn uses it to size the ELF buffer) ---
uint64_t fd_size(int fd) {
    FileHandle* h = fd_slot_get(fd);
    return (h == nullptr) ? 0 : h->node->size;
}

} // namespace VFS

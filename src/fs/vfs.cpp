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
    for (int i = 0; i < MAX_HANDLES; i++) handles[i].fs = nullptr;
}

// --- Path normalization ---
// Canonicalizes a path into 'out': absolute, no trailing slash, resolves
// '.' and '..'. Returns out.
static char* normalize(const char* path, char* out, uint64_t out_max) {
    // component starts, as offsets into out — on the stack, like every other
    // buffer on this path: a syscall can be preempted by the timer, so a static
    // would be shared with whatever task runs next.
    uint64_t starts[32];
    int depth = 0;
    uint64_t len = 0;

    out[0] = '\0';
    if (path == nullptr) return out;

    const char* p = path;
    if (*p != '/') {
        // relative path: caller treats it as relative to "/" for now
    }

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
            if (depth < 32 && len + clen + 2 < out_max) {
                starts[depth++] = len;
                out[len++] = '/';
                for (uint64_t i = 0; i < clen; i++) out[len++] = p[i];
                out[len] = '\0';
            }
        }
        p = end;
    }

    if (len == 0) { out[0] = '/'; out[1] = '\0'; }
    return out;
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
// (the caller owns that storage). Longest mount-prefix wins; relative paths
// resolve against the current task's CWD. Returns nullptr if nothing covers the
// path. Nothing here is shared between calls, so two resolutions can be live at
// once — rename() needs exactly that.
Filesystem* resolve(const char* path, char* rel_out, uint64_t rel_max) {
    char canon[256];
    normalize(path, canon, sizeof(canon));

    Mount* m = find_mount(canon);
    if (m == nullptr) {
        rel_out[0] = '\0';
        return nullptr;
    }

    uint64_t ml = 0;
    while (m->path[ml]) ml++;
    const char* rel = canon + ml;          // "/bin/ls.elf" -> "/ls.elf"
    if (*rel == '/') rel++;                // -> "ls.elf"; "/" -> ""

    uint64_t i = 0;
    while (rel[i] && i + 1 < rel_max) { rel_out[i] = rel[i]; i++; }
    rel_out[i] = '\0';
    return m->fs;
}

// --- CWD-aware path building ---
// Relative paths (no leading '/') resolve against the current task's CWD.
// strlen for internal use
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
    char rel[256];
    Filesystem* fs = resolve(full, rel, sizeof(rel));
    if (fs == nullptr) return false;
    if (fs->is_dir && fs->is_dir(fs->priv, rel)) return true;
    if (fs->open) {
        uint64_t sz = 0;
        void* h = fs->open(fs->priv, rel, VFS::O_RDONLY, &sz);
        if (h != nullptr) { fs->close(fs->priv, h); return true; }
    }
    return false;
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
    char cwdtry[256];
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

bool mount(const char* path, Filesystem* fs) {
    if (mount_count >= MAX_MOUNTS) return false;
    static char canon_storage[MAX_MOUNTS][128];
    normalize(path, canon_storage[mount_count], 128);
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
        if (parent && parent->fs->mkdir) {
            // strip the parent prefix -> fs-relative path (root's rel
            // paths have no leading slash; deeper mounts: skip the '/')
            const char* rel = mp + best_len;
            if (best_len > 1 && *rel == '/') rel++;
            parent->fs->mkdir(parent->fs->priv, rel);
        }
    }
    return true;
}

bool is_directory(const char* path) {
    char fixed[256];
    const char* effective = nullptr;
    resolve_path_fallback(path, fixed, sizeof(fixed), &effective, false);

    char rel[256];
    Filesystem* fs = resolve(effective, rel, sizeof(rel));
    if (fs == nullptr || fs->is_dir == nullptr) return false;
    return fs->is_dir(fs->priv, rel);
}

void canonicalize(const char* path, char* out, uint64_t out_max) {
    // Go through the same fallback resolution is_directory uses —
    // otherwise `cd Pictures` validates against /bin/Pictures but STORES
    // /Pictures (a nonexistent path), and every later ls/cd breaks.
    char fixed[256];
    const char* effective = nullptr;
    resolve_path_fallback(path, fixed, sizeof(fixed), &effective, false);
    normalize(effective, out, out_max);
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

int fd_slot_alloc(Filesystem* fs, void* fs_handle, uint64_t size) {
    FileHandle** fds = fds_of_current();
    // Start at FD_STD_STREAMS: slots 0..2 are reserved for the standard
    // streams, so the first descriptor a program opens is 3.
    for (int i = FD_STD_STREAMS; i < MAX_FDS; i++) {
        if (fds[i] == nullptr) {
            // grab a kernel-wide handle slot
            FileHandle* h = nullptr;
            for (int j = 0; j < MAX_HANDLES; j++) {
                if (handles[j].fs == nullptr) { h = &handles[j]; break; }
            }
            if (h == nullptr) return -1; // out of global handles

            h->fs = fs;
            h->fs_handle = fs_handle;
            h->offset = 0;
            h->size = size;
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
    char fixed[256];
    const char* effective = nullptr;
    // A creation is CWD-relative: it must not search /bin (see
    // resolve_path_fallback).
    resolve_path_fallback(path, fixed, sizeof(fixed), &effective,
                          (flags & O_CREATE) != 0);

    char rel[256];
    Filesystem* fs = resolve(effective, rel, sizeof(rel));
    if (fs == nullptr || fs->open == nullptr) return -Errno::ENOENT;

    uint64_t size = 0;
    void* h = fs->open(fs->priv, rel, flags, &size);
    if (h == nullptr) {
        // The backend refused. Callers care about the two common reasons:
        // a directory, or a creation the filesystem cannot do.
        if (flags & O_CREATE) return fs->write ? -Errno::EIO : -Errno::EROFS;
        if (fs->is_dir && fs->is_dir(fs->priv, rel)) return -Errno::EISDIR;
        return -Errno::ENOENT;
    }

    int fd = fd_slot_alloc(fs, h, size);
    if (fd == -1) {
        fs->close(fs->priv, h); // don't leak the backend handle
        return -Errno::EMFILE;
    }
    return fd;
}

long read_fd(int fd, void* buf, uint64_t count) {
    FileHandle* h = fd_slot_get(fd);
    if (h == nullptr) return -Errno::EBADF;
    if (h->fs->read == nullptr) return -Errno::EACCES;
    // Clamp against the file size reported at open (protects zero-copy
    // backends like tarfs whose read can't know its own bounds)
    if (h->offset >= h->size) return 0;
    if (count > h->size - h->offset) count = h->size - h->offset;
    uint64_t n = h->fs->read(h->fs->priv, h->fs_handle, buf, count, h->offset);
    h->offset += n;
    return (long)n;
}

long write_fd(int fd, const void* buf, uint64_t count) {
    FileHandle* h = fd_slot_get(fd);
    if (h == nullptr) return -Errno::EBADF;
    if (h->fs->write == nullptr) return -Errno::EROFS;
    uint64_t n = h->fs->write(h->fs->priv, h->fs_handle, buf, count, h->offset);
    h->offset += n;
    // The handle's size was captured at open; a write that extends the file
    // has to update it, or a later read (or lseek END) on the same fd would
    // still believe the old length.
    if (h->offset > h->size) h->size = h->offset;
    return (long)n;
}

void close_fd_for(FileHandle** fds, int fd) {
    if (fd < 0 || fd >= MAX_FDS) return;
    FileHandle* h = fds[fd];
    if (h == nullptr) return;
    if (h->fs && h->fs->close) h->fs->close(h->fs->priv, h->fs_handle);
    h->fs = nullptr;   // release the global handle slot
    h->fs_handle = nullptr;
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
    else if (whence == 2) base = (long)h->size;   // END
    else return -Errno::EINVAL;

    long pos = base + offset;
    if (pos < 0) pos = 0;
    if ((uint64_t)pos > h->size) pos = (long)h->size; // don't seek past EOF
    h->offset = (uint64_t)pos;
    return pos;
}

// Path rule (see vfs.h): lookups (open/stat) accept a bare name and search
// the CWD then /bin; creation and removal (open with O_CREATE, mkdir, unlink)
// are always CWD-relative, so a typo never lands in a program directory.
int mkdir(const char* path) {
    char rel[256];
    Filesystem* fs = resolve(path, rel, sizeof(rel));
    if (fs == nullptr) return -Errno::ENOENT;
    if (fs->mkdir == nullptr) return -Errno::EROFS;
    return fs->mkdir(fs->priv, rel) == 0 ? 0 : -Errno::EIO;
}

int unlink(const char* path) {
    char rel[256];
    Filesystem* fs = resolve(path, rel, sizeof(rel));
    if (fs == nullptr) return -Errno::ENOENT;
    if (fs->unlink == nullptr) return -Errno::EROFS;
    return fs->unlink(fs->priv, rel) == 0 ? 0 : -Errno::ENOENT;
}

// Rename resolves like mkdir/unlink: against the CWD (or an absolute path)
// only, never through the program search path. Both sides must be on the same
// filesystem — there is no cross-device move.
int rename(const char* from, const char* to) {
    char from_rel[256], to_rel[256];

    Filesystem* from_fs = resolve(from, from_rel, sizeof(from_rel));
    if (from_fs == nullptr) return -Errno::ENOENT;

    Filesystem* to_fs = resolve(to, to_rel, sizeof(to_rel));
    if (to_fs == nullptr) return -Errno::ENOENT;

    if (from_fs != to_fs) return -Errno::EINVAL;
    if (from_fs->rename == nullptr) return -Errno::EROFS;
    return from_fs->rename(from_fs->priv, from_rel, to_rel) == 0 ? 0 : -Errno::ENOENT;
}

int ftruncate_fd(int fd, uint64_t size) {
    FileHandle* h = fd_slot_get(fd);
    if (h == nullptr) return -Errno::EBADF;
    if (h->fs->truncate == nullptr) return -Errno::EROFS;
    int r = h->fs->truncate(h->fs->priv, h->fs_handle, size);
    if (r == 0) h->size = size;
    return r == 0 ? 0 : -Errno::EIO;
}

int stat_path(const char* path, uint64_t* out_size, bool* out_is_dir) {
    // A lookup: same resolution as open (CWD, then /bin for a bare name).
    char fixed[256];
    const char* effective = nullptr;
    resolve_path_fallback(path, fixed, sizeof(fixed), &effective, false);

    char rel[256];
    Filesystem* fs = resolve(effective, rel, sizeof(rel));
    if (fs == nullptr) return -Errno::ENOENT;

    if (fs->is_dir && fs->is_dir(fs->priv, rel)) {
        if (out_size) *out_size = 0;
        if (out_is_dir) *out_is_dir = true;
        return 0;
    }
    if (fs->open == nullptr) return -Errno::ENOENT;
    uint64_t size = 0;
    void* h = fs->open(fs->priv, rel, O_RDONLY, &size);
    if (h == nullptr) return -Errno::ENOENT;
    fs->close(fs->priv, h);
    if (out_size) *out_size = size;
    if (out_is_dir) *out_is_dir = false;
    return 0;
}

// --- Whole-file read (spawn + kernel use) ---
bool read_full(const char* path, char* buf, uint64_t max, uint64_t* out_size) {
    char fixed[256];
    const char* effective = nullptr;
    resolve_path_fallback(path, fixed, sizeof(fixed), &effective, false);

    char rel[256];
    Filesystem* fs = resolve(effective, rel, sizeof(rel));
    if (fs == nullptr || fs->open == nullptr) return false;

    uint64_t size = 0;
    void* h = fs->open(fs->priv, rel, O_RDONLY, &size);
    if (h == nullptr) return false;

    uint64_t n = (size < max) ? size : max;
    uint64_t got = fs->read(fs->priv, h, buf, n, 0);
    fs->close(fs->priv, h);
    if (out_size) *out_size = got;
    return true;
}

// --- readdir passthrough (syscall 2 shim) ---
uint64_t readdir(const char* path, char* buf, uint64_t max) {
    char fixed[256];
    const char* effective = nullptr;
    resolve_path_fallback(path, fixed, sizeof(fixed), &effective, false);

    char rel[256];
    Filesystem* fs = resolve(effective, rel, sizeof(rel));
    if (fs == nullptr || fs->readdir == nullptr) return 0;
    return fs->readdir(fs->priv, rel, buf, max);
}

// --- fd size accessor (spawn uses it to size the ELF buffer) ---
uint64_t fd_size(int fd) {
    FileHandle* h = fd_slot_get(fd);
    return (h == nullptr) ? 0 : h->size;
}

} // namespace VFS
#include "fs/overlay.h"
#include "fs/vfs.h"
#include "kernel/core/heap.h"

// See overlay.h for the model. A few notes on the mechanics:
//
//   * Paths arriving here are already mount-relative and have no leading slash
//     ("hello.txt", "fonts/bizcat.psf"); the root is "".
//   * A handle from either layer is wrapped in an OvHandle so read/write/close
//     can tell which backend it came from.
//   * Whiteouts are ordinary upper files named ".wh.<name>", the overlayfs
//     convention: cheap, needs no new mechanism, and readdir hides them.
//   * Buffers are on the kernel stack, not static: a syscall can be preempted
//     by the timer, and a static would then be shared with another task.

namespace Overlay {

static VFS::Filesystem* upper_fs = nullptr;
static VFS::Filesystem* lower_fs = nullptr;

// The handle the VFS sees: which backend, and that backend's own handle.
struct OvHandle {
    VFS::Filesystem* fs;
    void* h;
};

// --- small helpers -----------------------------------------------------

// "<dir>/<name>", or just <name> when dir is empty (the root).
static void join(const char* dir, const char* name, char* out, uint64_t max) {
    uint64_t n = 0;
    if (dir[0] != '\0') {
        for (uint64_t i = 0; dir[i] && n + 1 < max; i++) out[n++] = dir[i];
        if (n + 1 < max) out[n++] = '/';
    }
    for (uint64_t i = 0; name[i] && n + 1 < max; i++) out[n++] = name[i];
    out[n] = '\0';
}

// Does this backend have the path, and is it a directory?
static bool exists(VFS::Filesystem* fs, const char* path, bool* is_dir) {
    if (fs == nullptr) return false;
    if (fs->is_dir && fs->is_dir(fs->priv, path)) {
        if (is_dir) *is_dir = true;
        return true;
    }
    if (fs->open) {
        uint64_t size = 0;
        void* h = fs->open(fs->priv, path, VFS::O_RDONLY, &size);
        if (h != nullptr) {
            fs->close(fs->priv, h);
            if (is_dir) *is_dir = false;
            return true;
        }
    }
    return false;
}

// ".wh." + the final component of 'path' in the same directory.
static void whiteout_path(const char* path, char* out, uint64_t max) {
    int slash = -1;
    for (int i = 0; path[i]; i++) if (path[i] == '/') slash = i;
    uint64_t n = 0;
    for (int i = 0; i <= slash && n + 1 < max; i++) out[n++] = path[i];
    const char* pre = ".wh.";
    for (int i = 0; pre[i] && n + 1 < max; i++) out[n++] = pre[i];
    for (const char* p = path + slash + 1; *p && n + 1 < max; p++) out[n++] = *p;
    out[n] = '\0';
}

// Is a directory entry name a whiteout marker?
static bool name_is_whiteout(const char* name) {
    return name[0] == '.' && name[1] == 'w' && name[2] == 'h' && name[3] == '.';
}

// Has 'path' been deleted from the lower layer?
static bool is_whited_out(const char* path) {
    char w[256];
    whiteout_path(path, w, sizeof(w));
    bool dir;
    return exists(upper_fs, w, &dir);
}

// Copy a lower file into the upper so it can be modified. Chunked, so the whole
// file never has to sit in kernel heap.
static bool copy_up(const char* path) {
    uint64_t size = 0;
    void* lh = lower_fs->open(lower_fs->priv, path, VFS::O_RDONLY, &size);
    if (lh == nullptr) return false;

    void* uh = upper_fs->open(upper_fs->priv, path,
                              VFS::O_WRONLY | VFS::O_CREATE | VFS::O_TRUNC, &size);
    if (uh == nullptr) {
        lower_fs->close(lower_fs->priv, lh);
        return false;
    }

    char buf[1024];
    uint64_t off = 0;
    while (off < size) {
        uint64_t want = size - off;
        if (want > sizeof(buf)) want = sizeof(buf);
        uint64_t got = lower_fs->read(lower_fs->priv, lh, buf, want, off);
        if (got == 0) break;
        upper_fs->write(upper_fs->priv, uh, buf, got, off);
        off += got;
    }

    lower_fs->close(lower_fs->priv, lh);
    upper_fs->close(upper_fs->priv, uh);
    return true;
}

// --- the ops -----------------------------------------------------------

static void* ov_open(void* priv, const char* path, int flags, uint64_t* out_size) {
    (void)priv;
    const int writing = VFS::O_WRONLY | VFS::O_RDWR | VFS::O_CREATE | VFS::O_TRUNC;
    bool dir = false;

    VFS::Filesystem* fs;
    void* h;

    if (flags & writing) {
        // Writes always land in the upper. A lower file is copied up first so
        // that an O_RDWR does not silently lose its contents; O_TRUNC does not
        // need them, and the fresh upper file shadows the lower one anyway.
        if (!exists(upper_fs, path, &dir)) {
            if (lower_fs && exists(lower_fs, path, &dir)) {
                if (dir) return nullptr;                  // not openable
                if (!(flags & VFS::O_TRUNC) && !copy_up(path)) return nullptr;
            } else if (!(flags & VFS::O_CREATE)) {
                return nullptr;                           // no such file
            }
        }
        fs = upper_fs;
        h = upper_fs->open(upper_fs->priv, path, flags, out_size);
    } else {
        if (exists(upper_fs, path, &dir)) {
            if (dir) return nullptr;
            fs = upper_fs;
            h = upper_fs->open(upper_fs->priv, path, flags, out_size);
        } else if (lower_fs && !is_whited_out(path) &&
                   exists(lower_fs, path, &dir) && !dir) {
            fs = lower_fs;
            h = lower_fs->open(lower_fs->priv, path, flags, out_size);
        } else {
            return nullptr;
        }
    }

    if (h == nullptr) return nullptr;

    OvHandle* oh = new OvHandle;
    if (oh == nullptr) { fs->close(fs->priv, h); return nullptr; }
    oh->fs = fs;
    oh->h = h;
    return oh;
}

static uint64_t ov_read(void* priv, void* handle, void* buf,
                        uint64_t count, uint64_t offset) {
    (void)priv;
    OvHandle* oh = (OvHandle*)handle;
    return oh->fs->read(oh->fs->priv, oh->h, buf, count, offset);
}

static uint64_t ov_write(void* priv, void* handle, const void* buf,
                         uint64_t count, uint64_t offset) {
    (void)priv;
    OvHandle* oh = (OvHandle*)handle;
    if (oh->fs->write == nullptr) return 0;
    return oh->fs->write(oh->fs->priv, oh->h, buf, count, offset);
}

static void ov_close(void* priv, void* handle) {
    (void)priv;
    OvHandle* oh = (OvHandle*)handle;
    oh->fs->close(oh->fs->priv, oh->h);
    delete oh;
}

static int ov_truncate(void* priv, void* handle, uint64_t size) {
    (void)priv;
    OvHandle* oh = (OvHandle*)handle;
    if (oh->fs->truncate == nullptr) return -1;
    return oh->fs->truncate(oh->fs->priv, oh->h, size);
}

// Emit one listing line, if it fits.
static void emit(char* buf, uint64_t max, uint64_t* written, const char* name) {
    for (uint64_t i = 0; name[i]; i++) {
        if (*written + 3 >= max) return;
        buf[(*written)++] = name[i];
    }
    if (*written + 3 < max) buf[(*written)++] = '\n';
}

static uint64_t ov_readdir(void* priv, const char* path, char* buf, uint64_t max) {
    (void)priv;
    char ubuf[2048], lbuf[2048];
    ubuf[0] = '\0';
    lbuf[0] = '\0';
    if (upper_fs->readdir) upper_fs->readdir(upper_fs->priv, path, ubuf, sizeof(ubuf));
    if (lower_fs && lower_fs->readdir) lower_fs->readdir(lower_fs->priv, path, lbuf, sizeof(lbuf));

    uint64_t written = 0;

    // The upper's entries, minus the whiteout markers.
    for (const char* p = ubuf; *p; ) {
        const char* e = p;
        while (*e && *e != '\n') e++;
        char name[64];
        uint64_t n = 0;
        for (const char* q = p; q < e && n + 1 < sizeof(name); q++) name[n++] = *q;
        name[n] = '\0';
        if (!name_is_whiteout(name)) emit(buf, max, &written, name);
        p = *e ? e + 1 : e;
    }

    // The lower's entries that the upper does not already provide.
    for (const char* p = lbuf; *p; ) {
        const char* e = p;
        while (*e && *e != '\n') e++;
        char name[64];
        uint64_t n = 0;
        for (const char* q = p; q < e && n + 1 < sizeof(name); q++) name[n++] = *q;
        name[n] = '\0';

        char full[256];
        join(path, name, full, sizeof(full));
        bool dir;
        if (!exists(upper_fs, full, &dir) && !is_whited_out(full))
            emit(buf, max, &written, name);

        p = *e ? e + 1 : e;
    }

    buf[written] = '\0';
    return written;
}

static bool ov_is_dir(void* priv, const char* path) {
    (void)priv;
    bool dir = false;
    if (exists(upper_fs, path, &dir)) return dir;   // the upper shadows entirely
    if (is_whited_out(path)) return false;
    if (lower_fs && exists(lower_fs, path, &dir)) return dir;
    return false;
}

static int ov_mkdir(void* priv, const char* path) {
    (void)priv;
    if (upper_fs->mkdir == nullptr) return -1;

    // Refuse a path that already exists in either layer: creating an empty
    // upper directory over a lower one would hide its contents.
    bool dir = false;
    if (exists(upper_fs, path, &dir)) return -1;
    if (lower_fs && exists(lower_fs, path, &dir) && !is_whited_out(path)) return -1;

    return upper_fs->mkdir(upper_fs->priv, path);
}

static int ov_unlink(void* priv, const char* path) {
    (void)priv;
    if (upper_fs->unlink == nullptr) return -1;

    bool dir = false;
    if (exists(upper_fs, path, &dir)) return upper_fs->unlink(upper_fs->priv, path);

    // Only in the lower: leave a whiteout so it does not come back.
    if (lower_fs && exists(lower_fs, path, &dir)) {
        char w[256];
        whiteout_path(path, w, sizeof(w));
        uint64_t size = 0;
        void* h = upper_fs->open(upper_fs->priv, w,
                                 VFS::O_WRONLY | VFS::O_CREATE | VFS::O_TRUNC, &size);
        if (h == nullptr) return -1;
        upper_fs->close(upper_fs->priv, h);
        return 0;
    }
    return -1;
}

static int ov_rename(void* priv, const char* from, const char* to) {
    (void)priv;
    if (upper_fs->rename == nullptr) return -1;

    bool dir = false;
    if (!exists(upper_fs, from, &dir)) {
        if (!(lower_fs && exists(lower_fs, from, &dir))) return -1;
        if (dir) return -1;                     // renaming a lower directory
        if (!copy_up(from)) return -1;
    }

    // An existing lower destination is fine: the upper entry shadows it.
    return upper_fs->rename(upper_fs->priv, from, to);
}

static VFS::Filesystem fs = {
    .fs_type  = "overlay",
    .priv     = nullptr,
    .open     = ov_open,
    .read     = ov_read,
    .write    = ov_write,
    .close    = ov_close,
    .readdir  = ov_readdir,
    .is_dir   = ov_is_dir,
    .mkdir    = ov_mkdir,
    .truncate = ov_truncate,
    .unlink   = ov_unlink,
    .rename   = ov_rename,
};

void init(VFS::Filesystem* upper, VFS::Filesystem* lower) {
    upper_fs = upper;
    lower_fs = lower;
}

VFS::Filesystem* instance() {
    return &fs;
}

} // namespace Overlay

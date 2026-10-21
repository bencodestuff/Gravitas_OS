#include "fs/overlay.h"
#include "fs/vfs.h"
#include "kernel/core/heap.h"

// See overlay.h for the model. A few notes on the mechanics:
//
//   * Paths arriving here are already mount-relative and have no leading slash
//     ("hello.txt", "fonts/bizcat.psf"); the root is "".
//   * A node remembers both backends' views of the same entry, plus its path —
//     the path is what a copy-up and a whiteout need, and the two views are
//     what a merged readdir needs.
//   * Writes happen in the upper. A lower file is copied up on first write
//     (not at open: the node layer has no open, and a read-only open of a
//     lower file should not cost a copy).
//   * Whiteouts are ordinary upper files named ".wh.<name>", the overlayfs
//     convention: cheap, needs no new mechanism, and readdir hides them.
//   * Buffers are on the kernel stack, not static: a syscall can be preempted
//     by the timer, and a static would then be shared with another task.

namespace Overlay {

static VFS::Filesystem* upper_fs = nullptr;
static VFS::Filesystem* lower_fs = nullptr;

// What an overlay node holds.
struct OvNode {
    char path[256];
    VFS::Node* upper;   // the upper's view, or nullptr
    VFS::Node* lower;   // the lower's view, or nullptr
};

// --- the ops (prototyped so the table below can name them) ---
static VFS::Node* ov_root(void* priv);
static VFS::Node* ov_lookup(void* priv, VFS::Node* dir, const char* name);
static bool ov_readdir(void* priv, VFS::Node* dir, uint64_t index, VFS::DirEntry* out);
static uint64_t ov_read(void* priv, VFS::Node* node, void* buf, uint64_t count, uint64_t offset);
static uint64_t ov_write(void* priv, VFS::Node* node, const void* buf, uint64_t count, uint64_t offset);
static int ov_truncate(void* priv, VFS::Node* node, uint64_t size);
static int ov_create(void* priv, VFS::Node* dir, const char* name, VFS::FileType type, VFS::Node** out);
static int ov_unlink(void* priv, VFS::Node* dir, const char* name);
static int ov_rename(void* priv, VFS::Node* from_dir, const char* from, VFS::Node* to_dir, const char* to);
static void ov_release(void* priv, VFS::Node* node);
static int ov_map(void* priv, VFS::Node* node, uint64_t* out_phys, uint64_t* out_size);
static int ov_statfs(void* priv, VFS::StatFs* out);

static VFS::Filesystem fs = {
    .fs_type  = "overlay",
    .priv     = nullptr,
    .root     = ov_root,
    .lookup   = ov_lookup,
    .readdir  = ov_readdir,
    .read      = ov_read,
    .write     = ov_write,
    .truncate = ov_truncate,
    .create   = ov_create,
    .unlink   = ov_unlink,
    .rename   = ov_rename,
    .release  = ov_release,
    .statfs   = ov_statfs,
    .map      = ov_map,
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

// ".wh." + name; false when that would not fit.
static bool whiteout_name(const char* name, char* out, uint64_t max) {
    const char* pre = ".wh.";
    uint64_t n = 0;
    for (int i = 0; pre[i]; i++) { if (n + 1 >= max) return false; out[n++] = pre[i]; }
    for (uint64_t i = 0; name[i]; i++) { if (n + 1 >= max) return false; out[n++] = name[i]; }
    out[n] = '\0';
    return true;
}

// Is a directory entry name a whiteout marker?
static bool name_is_whiteout(const char* name) {
    return name[0] == '.' && name[1] == 'w' && name[2] == 'h' && name[3] == '.';
}

// Has 'name' in this upper directory been deleted from the lower?
static bool whited_out(VFS::Node* upper_dir, const char* name) {
    if (upper_dir == nullptr) return false;
    char wn[VFS::NAME_MAX];
    if (!whiteout_name(name, wn, sizeof(wn))) return false;
    VFS::Node* w = upper_fs->lookup(upper_fs->priv, upper_dir, wn);
    if (w == nullptr) return false;
    upper_fs->release(upper_fs->priv, w);
    return true;
}

// Look up one name in a backend directory (null-safe).
static VFS::Node* child_of(VFS::Filesystem* f, VFS::Node* dir, const char* name) {
    if (f == nullptr || dir == nullptr) return nullptr;
    return f->lookup(f->priv, dir, name);
}

// Wrap the two backends' views into an overlay node. Takes ownership of both
// views (releases them if it cannot build the node).
static VFS::Node* make_ov(const char* path, VFS::Node* upper, VFS::Node* lower) {
    VFS::Node* src = upper ? upper : lower;
    VFS::FileType type = src ? src->type : VFS::FT_FILE;

    VFS::Node* n = VFS::node_new(&fs, type);
    OvNode* on = (n != nullptr) ? new OvNode : nullptr;
    if (n == nullptr || on == nullptr) {
        delete n;
        if (upper) upper_fs->release(upper_fs->priv, upper);
        if (lower) lower_fs->release(lower_fs->priv, lower);
        return nullptr;
    }

    uint64_t i = 0;
    while (path[i] && i < sizeof(on->path) - 1) { on->path[i] = path[i]; i++; }
    on->path[i] = '\0';
    on->upper = upper;
    on->lower = lower;

    n->priv = on;
    if (src) {
        n->size = src->size;
        n->ino = src->ino;
        n->mode = src->mode;
        n->mtime = src->mtime;
    }
    return n;
}

// Make sure the upper has a directory to work in, creating it if the directory
// exists only in the lower. Stores it in *slot (which the node owns).
static VFS::Node* ensure_dir(VFS::Node** slot, const char* path) {
    if (*slot != nullptr) return *slot;
    VFS::Node* d = VFS::node_create(upper_fs, path, VFS::FT_DIR);
    if (d != nullptr) *slot = d;
    return d;
}

// Drop a half-created upper entry. Releasing the node alone would not do: the
// entry is already in the upper tree, and a zero-length or short upper would
// shadow the lower file, which is exactly the truncation copy-up is meant to
// avoid. The entry was created a moment ago in a directory the overlay owns, so
// the unlink should succeed; if it does not, releasing the node is all that is
// left.
static void discard_upper(const char* path, VFS::Node* up) {
    char parent[VFS::PATH_MAX];
    char name[VFS::NAME_MAX];
    if (upper_fs->unlink != nullptr && VFS::node_split(path, parent, name)) {
        VFS::Node* dir = VFS::node_walk(upper_fs, parent);
        if (dir != nullptr) {
            upper_fs->unlink(upper_fs->priv, dir, name);
            upper_fs->release(upper_fs->priv, dir);
        }
    }
    upper_fs->release(upper_fs->priv, up);
}

// Copy a lower file's bytes into a freshly created upper entry. False when the
// lower could not be read in full or the upper would not take every byte: a
// full upper reports that by writing nothing, so a short copy is a real
// failure, not the end of the file.
static bool copy_up(OvNode* on, VFS::Node* up) {
    if (on->lower == nullptr || on->lower->type != VFS::FT_FILE) return true;
    if (on->lower->size == 0) return true;
    if (on->lower->fs->read == nullptr || up->fs->write == nullptr) return false;

    char buf[1024];
    uint64_t off = 0;
    while (off < on->lower->size) {
        uint64_t want = on->lower->size - off;
        if (want > sizeof(buf)) want = sizeof(buf);
        uint64_t got = on->lower->fs->read(on->lower->fs->priv, on->lower,
                                          buf, want, off);
        if (got == 0 || got > want) return false;
        uint64_t put = up->fs->write(up->fs->priv, up, buf, got, off);
        if (put != got) return false;
        off += got;
    }
    return true;
}

// Give an entry an upper counterpart, copying a lower file's bytes up with it.
// The copy either lands whole or not at all: publishing a partial upper would
// make the missing tail read as zeros for the rest of the session, and the
// caller (a write) can then report ENOSPC instead.
static bool make_upper(OvNode* on) {
    if (on->upper != nullptr) return true;
    if (upper_fs == nullptr || upper_fs->create == nullptr) return false;

    VFS::Node* up = VFS::node_create(upper_fs, on->path, VFS::FT_FILE);
    if (up == nullptr) return false;

    if (!copy_up(on, up)) {
        discard_upper(on->path, up);
        return false;
    }
    on->upper = up;
    return true;
}

// --- the ops -----------------------------------------------------------

static VFS::Node* ov_root(void* priv) {
    (void)priv;
    VFS::Node* u = upper_fs->root(upper_fs->priv);
    VFS::Node* l = lower_fs ? lower_fs->root(lower_fs->priv) : nullptr;
    return make_ov("", u, l);
}

static VFS::Node* ov_lookup(void* priv, VFS::Node* dir, const char* name) {
    (void)priv;
    OvNode* d = (OvNode*)dir->priv;
    if (name[0] == '\0' || name_is_whiteout(name)) return nullptr;

    VFS::Node* u = child_of(upper_fs, d->upper, name);
    VFS::Node* l = nullptr;
    if (!whited_out(d->upper, name))
        l = child_of(lower_fs, d->lower, name);
    if (u == nullptr && l == nullptr) return nullptr;

    char path[256];
    join(d->path, name, path, sizeof(path));
    return make_ov(path, u, l);
}

static bool ov_readdir(void* priv, VFS::Node* dir, uint64_t index, VFS::DirEntry* out) {
    (void)priv;
    OvNode* d = (OvNode*)dir->priv;

    // The index counts *visible* entries: the upper's, minus whiteout markers,
    // then the lower's that the upper does not already provide or hide.
    uint64_t visible = 0;

    if (d->upper) {
        VFS::DirEntry e;
        for (uint64_t i = 0; upper_fs->readdir(upper_fs->priv, d->upper, i, &e); i++) {
            if (name_is_whiteout(e.name)) continue;
            if (visible == index) { *out = e; return true; }
            visible++;
        }
    }

    if (d->lower) {
        VFS::DirEntry e;
        for (uint64_t i = 0; lower_fs->readdir(lower_fs->priv, d->lower, i, &e); i++) {
            bool shadowed = false;
            if (d->upper) {
                VFS::Node* u = upper_fs->lookup(upper_fs->priv, d->upper, e.name);
                if (u) { shadowed = true; upper_fs->release(upper_fs->priv, u); }
            }
            if (shadowed || whited_out(d->upper, e.name)) continue;
            if (visible == index) { *out = e; return true; }
            visible++;
        }
    }
    return false;
}

static uint64_t ov_read(void* priv, VFS::Node* node, void* buf,
                        uint64_t count, uint64_t offset) {
    (void)priv;
    OvNode* on = (OvNode*)node->priv;
    VFS::Node* src = on->upper ? on->upper : on->lower;
    if (src == nullptr || src->fs->read == nullptr) return 0;
    return src->fs->read(src->fs->priv, src, buf, count, offset);
}

static uint64_t ov_write(void* priv, VFS::Node* node, const void* buf,
                         uint64_t count, uint64_t offset) {
    (void)priv;
    OvNode* on = (OvNode*)node->priv;
    if (!make_upper(on)) return 0;
    if (on->upper->fs->write == nullptr) return 0;
    return on->upper->fs->write(on->upper->fs->priv, on->upper, buf, count, offset);
}

static int ov_truncate(void* priv, VFS::Node* node, uint64_t size) {
    (void)priv;
    OvNode* on = (OvNode*)node->priv;
    if (!make_upper(on)) return -1;
    if (on->upper->fs->truncate == nullptr) return -1;
    return on->upper->fs->truncate(on->upper->fs->priv, on->upper, size);
}

static int ov_create(void* priv, VFS::Node* dir, const char* name,
                     VFS::FileType type, VFS::Node** out) {
    (void)priv;
    OvNode* d = (OvNode*)dir->priv;
    if (name[0] == '\0' || upper_fs->create == nullptr) return -1;

    if (ensure_dir(&d->upper, d->path) == nullptr) return -1;

    // Creating a directory over a lower one would hide the lower's contents.
    if (type == VFS::FT_DIR && d->lower && !whited_out(d->upper, name)) {
        VFS::Node* l = lower_fs->lookup(lower_fs->priv, d->lower, name);
        if (l) { lower_fs->release(lower_fs->priv, l); return -1; }
    }

    VFS::Node* un = nullptr;
    if (upper_fs->create(upper_fs->priv, d->upper, name, type, &un) != 0) return -1;

    char path[256];
    join(d->path, name, path, sizeof(path));
    VFS::Node* n = make_ov(path, un, nullptr);
    if (n == nullptr) return -1;
    *out = n;
    return 0;
}

static int ov_unlink(void* priv, VFS::Node* dir, const char* name) {
    (void)priv;
    OvNode* d = (OvNode*)dir->priv;
    if (name[0] == '\0' || upper_fs->unlink == nullptr) return -1;

    if (d->upper) {
        VFS::Node* u = upper_fs->lookup(upper_fs->priv, d->upper, name);
        if (u) {
            upper_fs->release(upper_fs->priv, u);
            return upper_fs->unlink(upper_fs->priv, d->upper, name);
        }
    }

    // Only in the lower: leave a whiteout so it does not come back.
    VFS::Node* l = child_of(lower_fs, d->lower, name);
    if (l == nullptr) return -1;
    lower_fs->release(lower_fs->priv, l);

    if (ensure_dir(&d->upper, d->path) == nullptr) return -1;

    char wn[VFS::NAME_MAX];
    if (!whiteout_name(name, wn, sizeof(wn))) return -1;

    VFS::Node* w = upper_fs->lookup(upper_fs->priv, d->upper, wn);
    if (w) { upper_fs->release(upper_fs->priv, w); return 0; }  // already hidden

    if (upper_fs->create(upper_fs->priv, d->upper, wn, VFS::FT_FILE, &w) != 0)
        return -1;
    upper_fs->release(upper_fs->priv, w);
    return 0;
}

static int ov_rename(void* priv, VFS::Node* from_dir, const char* from,
                     VFS::Node* to_dir, const char* to) {
    (void)priv;
    OvNode* fd = (OvNode*)from_dir->priv;
    OvNode* td = (OvNode*)to_dir->priv;
    if (from[0] == '\0' || to[0] == '\0') return -1;
    if (upper_fs->rename == nullptr) return -1;

    VFS::Node* fup = ensure_dir(&fd->upper, fd->path);
    if (fup == nullptr) return -1;

    bool in_upper = false;
    VFS::Node* u = upper_fs->lookup(upper_fs->priv, fup, from);
    if (u) { in_upper = true; upper_fs->release(upper_fs->priv, u); }

    if (!in_upper) {
        VFS::Node* l = child_of(lower_fs, fd->lower, from);
        if (l == nullptr) return -1;
        if (l->type == VFS::FT_DIR) { lower_fs->release(lower_fs->priv, l); return -1; }

        OvNode tmp;
        join(fd->path, from, tmp.path, sizeof(tmp.path));
        tmp.upper = nullptr;
        tmp.lower = l;
        bool ok = make_upper(&tmp);
        if (tmp.upper) upper_fs->release(upper_fs->priv, tmp.upper);
        lower_fs->release(lower_fs->priv, l);
        if (!ok) return -1;
    }

    VFS::Node* tup = ensure_dir(&td->upper, td->path);
    if (tup == nullptr) return -1;

    // An existing lower destination is fine: the upper entry shadows it.
    return upper_fs->rename(upper_fs->priv, fup, from, tup, to);
}

static void ov_release(void* priv, VFS::Node* node) {
    (void)priv;
    OvNode* on = (OvNode*)node->priv;
    if (on->upper) upper_fs->release(upper_fs->priv, on->upper);
    if (on->lower) lower_fs->release(lower_fs->priv, on->lower);
    delete on;
    delete node;
}

// Only the image can promise a file's bytes stay put, so a mapping is only
// possible for a file that still lives in the lower layer: an upper copy is a
// ramfs buffer that may be reallocated under the mapping.
static int ov_map(void* priv, VFS::Node* node, uint64_t* out_phys,
                  uint64_t* out_size) {
    (void)priv;
    OvNode* on = (OvNode*)node->priv;
    if (on->upper) return -1;
    if (on->lower == nullptr || on->lower->fs->map == nullptr) return -1;
    return on->lower->fs->map(on->lower->fs->priv, on->lower, out_phys, out_size);
}

// The overlay's room is the upper layer's room: that is where a write lands.
// (The lower is read-only, so its size would be a number nothing can spend.)
static int ov_statfs(void* priv, VFS::StatFs* out) {
    (void)priv;
    out->writable = (upper_fs != nullptr && upper_fs->write != nullptr);
    out->free = 0;
    out->total = 0;
    if (upper_fs != nullptr && upper_fs->statfs != nullptr)
        upper_fs->statfs(upper_fs->priv, out);
    return 0;
}

void init(VFS::Filesystem* upper, VFS::Filesystem* lower) {
    upper_fs = upper;
    lower_fs = lower;
}

VFS::Filesystem* instance() {
    return &fs;
}

} // namespace Overlay

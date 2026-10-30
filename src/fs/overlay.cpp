#include "fs/overlay.h"
#include "fs/vfs.h"
#include "kernel/core/errno.h"
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
//   * The invariant the delete and move paths enforce: an upper entry always
//     shadows the lower one, and removing or moving a name that exists in
//     either layer leaves a ".wh.<name>" marker in the upper. Without the
//     marker the lower entry reappears as soon as the upper stops supplying
//     the name, which is exactly what a delete after a copy-up would do.
//   * ".wh." is reserved: a create or a rename destination whose name starts
//     with it is refused, so a real file can never look like a marker.
//   * Paths are carried whole or refused with ENAMETOOLONG. A truncated path
//     names a different file, and a copy-up or a whiteout would then land on
//     that other name.
//   * Buffers are on the kernel stack, not static: a syscall can be preempted
//     by the timer, and a static would then be shared with another task.

namespace Overlay {

static VFS::Filesystem* upper_fs = nullptr;
static VFS::Filesystem* lower_fs = nullptr;

// What an overlay node holds.
struct OvNode {
    char path[VFS::PATH_MAX];
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
static int ov_sync(void* priv);

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
    .sync     = ov_sync,
};

// --- small helpers -----------------------------------------------------

// "<dir>/<name>", or just <name> when dir is empty (the root). False when the
// result does not fit: a cut-down path names a different entry, so every caller
// refuses rather than acts on the prefix.
static bool join(const char* dir, const char* name, char* out, uint64_t max) {
    uint64_t n = 0;
    if (dir[0] != '\0') {
        for (uint64_t i = 0; dir[i]; i++) {
            if (n + 1 >= max) return false;
            out[n++] = dir[i];
        }
        if (n + 1 >= max) return false;
        out[n++] = '/';
    }
    for (uint64_t i = 0; name[i]; i++) {
        if (n + 1 >= max) return false;
        out[n++] = name[i];
    }
    out[n] = '\0';
    return true;
}

// Copy a path into a fixed buffer; false when it does not fit.
static bool copy_path(char* out, uint64_t max, const char* path) {
    uint64_t i = 0;
    for (; path[i]; i++) {
        if (i + 1 >= max) return false;
        out[i] = path[i];
    }
    out[i] = '\0';
    return true;
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

// Leave ".wh.<name>" in an upper directory, so a lower entry of that name stays
// hidden. *created says whether this call made the marker rather than finding
// one already there: a failed rename has to remove only its own.
static int add_whiteout(VFS::Node* upper_dir, const char* name, bool* created) {
    if (created) *created = false;
    char wn[VFS::NAME_MAX];
    if (!whiteout_name(name, wn, sizeof(wn))) return -Errno::ENAMETOOLONG;
    if (upper_fs->create == nullptr) return -Errno::EROFS;

    VFS::Node* w = upper_fs->lookup(upper_fs->priv, upper_dir, wn);
    if (w != nullptr) { upper_fs->release(upper_fs->priv, w); return 0; }

    int r = upper_fs->create(upper_fs->priv, upper_dir, wn, VFS::FT_FILE, &w);
    if (r != 0) return r;
    upper_fs->release(upper_fs->priv, w);
    if (created) *created = true;
    return 0;
}

// Drop a whiteout this call created, to undo it after a failed move.
static void remove_whiteout(VFS::Node* upper_dir, const char* name) {
    char wn[VFS::NAME_MAX];
    if (upper_fs->unlink == nullptr) return;
    if (!whiteout_name(name, wn, sizeof(wn))) return;
    VFS::Node* w = upper_fs->lookup(upper_fs->priv, upper_dir, wn);
    if (w == nullptr) return;
    upper_fs->release(upper_fs->priv, w);
    upper_fs->unlink(upper_fs->priv, upper_dir, wn);
}

// Does this backend directory hold any entry at all?
static bool dir_has_entries(VFS::Filesystem* f, VFS::Node* dir) {
    if (f == nullptr || dir == nullptr || f->readdir == nullptr) return false;
    VFS::DirEntry e;
    return f->readdir(f->priv, dir, 0, &e);
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

    if (!copy_path(on->path, sizeof(on->path), path)) {
        if (upper) upper_fs->release(upper_fs->priv, upper);
        if (lower) lower_fs->release(lower_fs->priv, lower);
        delete on;
        delete n;
        return nullptr;
    }
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
// exists only in the lower. Stores it in *slot (which the node owns). A failure
// fills *out_err with the upper's reason (typically ENOSPC) so the caller can
// pass it on rather than invent one.
static VFS::Node* ensure_dir(VFS::Node** slot, const char* path, int* out_err) {
    if (out_err) *out_err = 0;
    if (*slot != nullptr) return *slot;
    int err = 0;
    VFS::Node* d = VFS::node_create(upper_fs, path, VFS::FT_DIR, &err);
    if (d != nullptr) *slot = d;
    else if (out_err) *out_err = err ? err : -Errno::EIO;
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

    char path[VFS::PATH_MAX];
    if (!join(d->path, name, path, sizeof(path))) return nullptr;
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
    if (name[0] == '\0' || upper_fs->create == nullptr) return -Errno::EINVAL;
    // ".wh." means "hidden in the lower layer": a user entry by that name could
    // never be opened and would collide with a marker.
    if (name_is_whiteout(name)) return -Errno::EINVAL;

    char path[VFS::PATH_MAX];
    if (!join(d->path, name, path, sizeof(path))) return -Errno::ENAMETOOLONG;

    int derr = 0;
    if (ensure_dir(&d->upper, d->path, &derr) == nullptr) return derr;

    // Creating a directory over a lower one would hide the lower's contents.
    if (type == VFS::FT_DIR && d->lower && !whited_out(d->upper, name)) {
        VFS::Node* l = lower_fs->lookup(lower_fs->priv, d->lower, name);
        if (l) { lower_fs->release(lower_fs->priv, l); return -Errno::EEXIST; }
    }

    VFS::Node* un = nullptr;
    // The upper owns the reason: ENOSPC when it is full, EEXIST when the name
    // appeared in the meantime. The overlay adds nothing of its own.
    int r = upper_fs->create(upper_fs->priv, d->upper, name, type, &un);
    if (r != 0) return r;

    VFS::Node* n = make_ov(path, un, nullptr);
    if (n == nullptr) return -Errno::ENOMEM;
    *out = n;
    return 0;
}

static int ov_unlink(void* priv, VFS::Node* dir, const char* name) {
    (void)priv;
    OvNode* d = (OvNode*)dir->priv;
    if (name[0] == '\0') return -Errno::EINVAL;
    if (name_is_whiteout(name)) return -Errno::EINVAL;   // a marker is not a file
    if (upper_fs->unlink == nullptr) return -Errno::EROFS;

    VFS::Node* u = child_of(upper_fs, d->upper, name);
    VFS::Node* l = whited_out(d->upper, name) ? nullptr
                                             : child_of(lower_fs, d->lower, name);
    if (u == nullptr && l == nullptr) return -Errno::ENOENT;

    bool lower_here = (l != nullptr);
    bool made = false;

    // The lower layer cannot lose an entry, so a lower directory can only be
    // hidden by a marker, and a marker over a non-empty directory makes the
    // whole subtree unreachable. unlink doubles as rmdir here: an empty lower
    // directory may be hidden, any directory with entries is refused.
    if (l != nullptr && l->type == VFS::FT_DIR) {
        bool empty = !dir_has_entries(lower_fs, l);
        lower_fs->release(lower_fs->priv, l);
        l = nullptr;
        if (!empty) {
            if (u) upper_fs->release(upper_fs->priv, u);
            return -Errno::EISDIR;
        }
    }

    // The marker goes in before the upper entry goes. A copy-up followed by a
    // delete is the case that used to resurrect the image's file, and a marker
    // that cannot be written must leave the name visible, not reveal the lower.
    if (lower_here) {
        int derr = 0;
        if (ensure_dir(&d->upper, d->path, &derr) == nullptr) {
            if (l) lower_fs->release(lower_fs->priv, l);
            return derr ? derr : -Errno::EIO;
        }

        int r = add_whiteout(d->upper, name, &made);
        if (r != 0) {
            if (l) lower_fs->release(lower_fs->priv, l);
            if (u) upper_fs->release(upper_fs->priv, u);
            return r;
        }
    }
    if (l) lower_fs->release(lower_fs->priv, l);

    if (u != nullptr) {
        upper_fs->release(upper_fs->priv, u);
        if (upper_fs->unlink(upper_fs->priv, d->upper, name) != 0) {
            if (made) remove_whiteout(d->upper, name);
            return -Errno::EIO;
        }
    }
    return 0;
}

static int ov_rename(void* priv, VFS::Node* from_dir, const char* from,
                     VFS::Node* to_dir, const char* to) {
    (void)priv;
    OvNode* fd = (OvNode*)from_dir->priv;
    OvNode* td = (OvNode*)to_dir->priv;
    if (from[0] == '\0' || to[0] == '\0') return -Errno::EINVAL;
    if (name_is_whiteout(from) || name_is_whiteout(to)) return -Errno::EINVAL;
    if (upper_fs->rename == nullptr) return -Errno::EROFS;

    char from_path[VFS::PATH_MAX];
    if (!join(fd->path, from, from_path, sizeof(from_path)))
        return -Errno::ENAMETOOLONG;

    VFS::Node* fup = ensure_dir(&fd->upper, fd->path, nullptr);
    VFS::Node* tup = ensure_dir(&td->upper, td->path, nullptr);
    if (fup == nullptr || tup == nullptr) return -Errno::EIO;

    // What the two layers hold under the old and the new name. A whited-out
    // lower name is not there as far as the overlay is concerned.
    VFS::Node* u = upper_fs->lookup(upper_fs->priv, fup, from);
    VFS::Node* l = whited_out(fup, from) ? nullptr
                                         : child_of(lower_fs, fd->lower, from);
    if (u == nullptr && l == nullptr) return -Errno::ENOENT;

    // A directory that the lower layer owns cannot move: its contents live in
    // the image, there is nowhere to copy a tree to, and whiteouting the old
    // name would put the whole subtree out of reach.
    if (l != nullptr && l->type == VFS::FT_DIR) {
        lower_fs->release(lower_fs->priv, l);
        if (u) upper_fs->release(upper_fs->priv, u);
        return -Errno::EISDIR;
    }
    bool lower_from = (l != nullptr);

    // Replacing a lower directory with a file would hide its subtree, so the
    // move is refused for the same reason unlink refuses one.
    bool lower_to = false;
    if (!whited_out(tup, to)) {
        VFS::Node* lt = child_of(lower_fs, td->lower, to);
        if (lt != nullptr) {
            lower_to = true;
            if (lt->type == VFS::FT_DIR) {
                lower_fs->release(lower_fs->priv, lt);
                if (l) lower_fs->release(lower_fs->priv, l);
                if (u) upper_fs->release(upper_fs->priv, u);
                return -Errno::EISDIR;
            }
            lower_fs->release(lower_fs->priv, lt);
        }
    }

    // The upper backend can only move what it holds: a lower-only source is
    // copied up first, exactly as a write would.
    if (u == nullptr) {
        OvNode tmp;
        if (!copy_path(tmp.path, sizeof(tmp.path), from_path)) {
            lower_fs->release(lower_fs->priv, l);
            return -Errno::ENAMETOOLONG;
        }
        tmp.upper = nullptr;
        tmp.lower = l;
        bool ok = make_upper(&tmp);
        if (tmp.upper) upper_fs->release(upper_fs->priv, tmp.upper);
        if (!ok) {
            lower_fs->release(lower_fs->priv, l);
            return -Errno::EIO;
        }
    }

    // Both markers go in before the move, so a move that cannot be recorded as
    // a removal is refused rather than leaving the lower name visible. A failed
    // move puts back only the markers this call created.
    bool made_from = false, made_to = false;
    int r = 0;
    if (lower_from) r = add_whiteout(fup, from, &made_from);
    if (r == 0 && lower_to) r = add_whiteout(tup, to, &made_to);
    if (r == 0) {
        r = upper_fs->rename(upper_fs->priv, fup, from, tup, to);
        if (r != 0) r = -Errno::EIO;
    }
    if (r != 0) {
        if (made_from) remove_whiteout(fup, from);
        if (made_to) remove_whiteout(tup, to);
    }
    if (l) lower_fs->release(lower_fs->priv, l);
    if (u) upper_fs->release(upper_fs->priv, u);
    return r;
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

// The overlay has no cache of its own: durability belongs to the layer the
// bytes actually live in, which is the upper for everything writable.
static int ov_sync(void* priv) {
    (void)priv;
    if (upper_fs == nullptr || upper_fs->sync == nullptr) return 0;
    return upper_fs->sync(upper_fs->priv);
}

void init(VFS::Filesystem* upper, VFS::Filesystem* lower) {
    upper_fs = upper;
    lower_fs = lower;
}

VFS::Filesystem* instance() {
    return &fs;
}

} // namespace Overlay

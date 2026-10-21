#include "fs/ramfs.h"
#include "fs/vfs.h"
#include "kernel/core/heap.h"
#include "kernel/dev/rtc.h"

extern "C" void kprint(const char* str);

// --- RamFS: an in-memory filesystem with create/write support ---
// A tree of nodes; file data lives on the kernel heap and grows as needed.
// Mounted at "/" at boot (through the overlay) and usable for /tmp etc.
//
// A RamNode is the durable thing (it is what the tree holds); a VFS::Node is a
// short-lived window onto it that a caller holds while it works. That split is
// what makes unlink-while-open safe: the directory entry goes away, the bytes a
// reader is using do not, because the open handle still holds a reference.

namespace RamFS {

struct RamNode {
    char name[64];
    bool is_dir;
    uint8_t* data;
    uint64_t size;
    uint64_t capacity;
    int refs;              // 1 for being in the tree, +1 per live VFS::Node
    uint32_t ino;
    uint64_t mtime;
    RamNode* first_child;
    RamNode* next_sibling;
};

static RamNode* root = nullptr;
static uint32_t next_ino = 1;

// A ceiling on file data, because the backing store is the kernel heap: a
// runaway writer would otherwise eat all 32MB of it and there would be no
// ENOSPC to report. Per file, and across the filesystem.
constexpr uint64_t MAX_FILE_BYTES  = 1ull << 20; // 1 MB
constexpr uint64_t MAX_TOTAL_BYTES = 8ull << 20; // 8 MB
static uint64_t ram_bytes = 0;                   // held by file data right now

// Give a file a buffer of at least 'need' bytes, growing in pages. False when
// that would pass a cap (the caller then reports ENOSPC by writing less).
static bool reserve(RamNode* n, uint64_t need) {
    if (need <= n->capacity) return true;
    if (need > MAX_FILE_BYTES) return false;

    uint64_t newcap = (need + 4095) & ~(uint64_t)4095;
    // What is already held, less this file's old buffer, plus the new one.
    if (ram_bytes - n->capacity + newcap > MAX_TOTAL_BYTES) return false;

    uint8_t* nd = new uint8_t[newcap];
    if (nd == nullptr) return false;
    for (uint64_t i = 0; i < n->size; i++) nd[i] = n->data[i];
    // The heap recycles memory between tasks, so the bytes past the copy must
    // not keep a previous owner's contents: every byte of a file that was never
    // written has to read as zero.
    for (uint64_t i = n->size; i < newcap; i++) nd[i] = 0;
    delete[] n->data;
    n->data = nd;
    ram_bytes += newcap - n->capacity;
    n->capacity = newcap;
    return true;
}

// --- node helpers ---
static RamNode* new_node(const char* name, bool is_dir) {
    RamNode* n = new RamNode;
    int i = 0;
    while (name[i] && i < (int)sizeof(n->name) - 1) { n->name[i] = name[i]; i++; }
    n->name[i] = '\0';
    n->is_dir = is_dir;
    n->data = nullptr;
    n->size = 0;
    n->capacity = 0;
    n->refs = 1;           // the tree holds one reference
    n->ino = next_ino++;
    n->mtime = Rtc::seconds_now();
    n->first_child = nullptr;
    n->next_sibling = nullptr;
    return n;
}

// Drop a reference; the node is freed when the tree and every live VFS::Node
// have let go.
static void node_unref(RamNode* n) {
    if (--n->refs > 0) return;
    if (n->data) {
        ram_bytes -= n->capacity;
        delete[] n->data;
    }
    delete n;
}

static RamNode* find_child(RamNode* dir, const char* name) {
    for (RamNode* c = dir->first_child; c; c = c->next_sibling) {
        const char* a = c->name;
        const char* b = name;
        bool eq = true;
        while (*a && *b) { if (*a != *b) { eq = false; break; } a++; b++; }
        if (eq && *a == '\0' && *b == '\0') return c;
    }
    return nullptr;
}

// Unlink a child from its parent's child list.
static void detach(RamNode* dir, RamNode* node) {
    RamNode* prev = nullptr;
    for (RamNode* c = dir->first_child; c; prev = c, c = c->next_sibling) {
        if (c != node) continue;
        if (prev) prev->next_sibling = c->next_sibling;
        else dir->first_child = c->next_sibling;
        return;
    }
}

// --- the VFS ops ---
// A node is a window onto a RamNode: it takes a reference, and release() gives
// it back. The durable state is the RamNode.
static VFS::Node* wrap(RamNode* rn);

static VFS::Node* ram_root(void* priv);
static VFS::Node* ram_lookup(void* priv, VFS::Node* dir, const char* name);
static bool ram_readdir(void* priv, VFS::Node* dir, uint64_t index, VFS::DirEntry* out);
static uint64_t ram_read(void* priv, VFS::Node* node, void* buf, uint64_t count, uint64_t offset);
static uint64_t ram_write(void* priv, VFS::Node* node, const void* buf, uint64_t count, uint64_t offset);
static int ram_truncate(void* priv, VFS::Node* node, uint64_t size);
static int ram_create(void* priv, VFS::Node* dir, const char* name, VFS::FileType type, VFS::Node** out);
static int ram_unlink(void* priv, VFS::Node* dir, const char* name);
static int ram_rename(void* priv, VFS::Node* from_dir, const char* from, VFS::Node* to_dir, const char* to);
static void ram_release(void* priv, VFS::Node* node);
static int ram_statfs(void* priv, VFS::StatFs* out);

static VFS::Filesystem fs = {
    .fs_type  = "ramfs",
    .priv     = nullptr,
    .root     = ram_root,
    .lookup   = ram_lookup,
    .readdir  = ram_readdir,
    .read      = ram_read,
    .write     = ram_write,
    .truncate = ram_truncate,
    .create   = ram_create,
    .unlink   = ram_unlink,
    .rename   = ram_rename,
    .release  = ram_release,
    .statfs   = ram_statfs,
    .map      = nullptr, // its bytes can be reallocated, so nothing may map it
};

static VFS::Node* wrap(RamNode* rn) {
    VFS::Node* n = VFS::node_new(&fs, rn->is_dir ? VFS::FT_DIR : VFS::FT_FILE);
    if (n == nullptr) return nullptr;
    rn->refs++;
    n->priv = rn;
    n->size = rn->size;
    n->ino = rn->ino;
    n->mode = rn->is_dir ? 0755 : 0644;
    n->mtime = rn->mtime;
    return n;
}

static VFS::Node* ram_root(void* priv) {
    (void)priv;
    return wrap(root);
}

static VFS::Node* ram_lookup(void* priv, VFS::Node* dir, const char* name) {
    (void)priv;
    RamNode* d = (RamNode*)dir->priv;
    if (!d->is_dir) return nullptr;
    RamNode* c = find_child(d, name);
    if (c == nullptr) return nullptr;
    return wrap(c);
}

static bool ram_readdir(void* priv, VFS::Node* dir, uint64_t index, VFS::DirEntry* out) {
    (void)priv;
    RamNode* d = (RamNode*)dir->priv;
    if (!d->is_dir) return false;

    RamNode* c = d->first_child;
    for (uint64_t i = 0; c && i < index; i++) c = c->next_sibling;
    if (c == nullptr) return false;

    uint64_t n = 0;
    while (c->name[n] && n + 1 < sizeof(out->name)) { out->name[n] = c->name[n]; n++; }
    out->name[n] = '\0';
    out->type = c->is_dir ? VFS::FT_DIR : VFS::FT_FILE;
    out->ino = c->ino;
    return true;
}

static uint64_t ram_read(void* priv, VFS::Node* node, void* buf,
                         uint64_t count, uint64_t offset) {
    (void)priv;
    RamNode* n = (RamNode*)node->priv;
    if (offset >= n->size) return 0;
    uint64_t avail = n->size - offset;
    uint64_t nread = (count < avail) ? count : avail;
    uint8_t* src = n->data + offset;
    uint8_t* dst = (uint8_t*)buf;
    for (uint64_t i = 0; i < nread; i++) dst[i] = src[i];
    return nread;
}

static uint64_t ram_write(void* priv, VFS::Node* node, const void* buf,
                          uint64_t count, uint64_t offset) {
    (void)priv;
    RamNode* n = (RamNode*)node->priv;

    // Prove the range fits before computing it. 'offset + count' can wrap, and
    // a wrapped 'end' would pass the cap check and let the copy below run off
    // the end of the heap buffer. Subtracting instead of adding also rejects an
    // offset that itself lies past the cap.
    if (count > MAX_FILE_BYTES || offset > MAX_FILE_BYTES - count) return 0;
    uint64_t end = offset + count;

    // No room: report nothing written, which the VFS turns into ENOSPC. (Not a
    // partial write: the caps are page-sized, and a short write that silently
    // loses bytes is worse than a refusal.)
    if (!reserve(n, end)) return 0;

    // A write past the current end must leave a zero-filled hole, not whatever
    // the heap buffer (or a previous, larger allocation) happened to hold.
    if (offset > n->size) {
        for (uint64_t i = n->size; i < offset; i++) n->data[i] = 0;
    }

    const uint8_t* src = (const uint8_t*)buf;
    for (uint64_t i = 0; i < count; i++) n->data[offset + i] = src[i];
    if (end > n->size) n->size = end;
    n->mtime = Rtc::seconds_now();
    node->size = n->size;   // keep this window onto the file current
    return count;
}

static int ram_truncate(void* priv, VFS::Node* node, uint64_t size) {
    (void)priv;
    RamNode* n = (RamNode*)node->priv;
    if (size > n->capacity) {
        if (!reserve(n, size)) return -1; // past a cap
        for (uint64_t i = n->size; i < size; i++) n->data[i] = 0; // zero-fill
    } else if (size > n->size) {
        for (uint64_t i = n->size; i < size; i++) n->data[i] = 0;
    }
    n->size = size;
    n->mtime = Rtc::seconds_now();
    node->size = size;
    return 0;
}

static int ram_create(void* priv, VFS::Node* dir, const char* name,
                      VFS::FileType type, VFS::Node** out) {
    (void)priv;
    RamNode* d = (RamNode*)dir->priv;
    if (!d->is_dir || name[0] == '\0') return -1;
    if (find_child(d, name) != nullptr) return -1;   // already there

    RamNode* c = new_node(name, type == VFS::FT_DIR);
    c->next_sibling = d->first_child;
    d->first_child = c;

    VFS::Node* n = wrap(c);
    if (n == nullptr) { detach(d, c); node_unref(c); return -1; }
    *out = n;
    return 0;
}

static int ram_unlink(void* priv, VFS::Node* dir, const char* name) {
    (void)priv;
    RamNode* d = (RamNode*)dir->priv;
    if (!d->is_dir) return -1;

    RamNode* c = find_child(d, name);
    if (c == nullptr) return -1;
    if (c->is_dir && c->first_child) return -1;   // non-empty dir

    detach(d, c);
    node_unref(c); // the tree's reference; a live VFS::Node keeps the node alive
    return 0;
}

// Rename/move within the ramfs. Replaces an existing destination file; refuses
// to clobber a non-empty directory. (Moving a directory into its own subtree is
// not detected: nodes carry no parent pointer.)
static int ram_rename(void* priv, VFS::Node* from_dir_node, const char* from,
                      VFS::Node* to_dir_node, const char* to) {
    (void)priv;
    RamNode* fdir = (RamNode*)from_dir_node->priv;
    RamNode* tdir = (RamNode*)to_dir_node->priv;
    if (!fdir->is_dir || !tdir->is_dir) return -1;
    if (from[0] == '\0' || to[0] == '\0') return -1;

    RamNode* src = find_child(fdir, from);
    if (src == nullptr) return -1;

    RamNode* dst = find_child(tdir, to);
    if (dst != nullptr && dst != src) {
        if (dst->is_dir && dst->first_child) return -1;   // non-empty dir
        detach(tdir, dst);
        node_unref(dst); // as in unlink: a live VFS::Node keeps it alive
    }

    detach(fdir, src);

    int i = 0;
    while (to[i] && i < (int)sizeof(src->name) - 1) { src->name[i] = to[i]; i++; }
    src->name[i] = '\0';
    src->next_sibling = tdir->first_child;
    tdir->first_child = src;
    return 0;
}

static void ram_release(void* priv, VFS::Node* node) {
    (void)priv;
    if (node->priv) node_unref((RamNode*)node->priv);
    delete node;
}

// The only thing that knows the heap caps, so the only thing that can report
// them. This is what makes ENOSPC predictable rather than a surprise.
static int ram_statfs(void* priv, VFS::StatFs* out) {
    (void)priv;
    out->total = MAX_TOTAL_BYTES;
    out->free = MAX_TOTAL_BYTES - ram_bytes;
    out->writable = true;
    return 0;
}

VFS::Filesystem* instance() {
    return &fs;
}

void init() {
    root = new_node("", true);
    kprint("[RamFS] Ready.\n");
}

} // namespace RamFS

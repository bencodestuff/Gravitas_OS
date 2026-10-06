#include "fs/ramfs.h"
#include "fs/vfs.h"
#include "kernel/core/heap.h"

extern "C" void kprint(const char* str);

// --- RamFS: an in-memory filesystem with create/write support ---
// Tree of nodes; file data lives on the kernel heap and grows as needed.
// Mounted at "/" at boot (and usable for /tmp etc).

namespace RamFS {

struct Node {
    char name[32];
    bool is_dir;
    uint8_t* data;
    uint64_t size;
    uint64_t capacity;
    Node* first_child;
    Node* next_sibling;
};

static Node* root = nullptr;

// --- node helpers ---
static Node* new_node(const char* name, bool is_dir) {
    Node* n = new Node;
    int i = 0;
    while (name[i] && i < 31) { n->name[i] = name[i]; i++; }
    n->name[i] = '\0';
    n->is_dir = is_dir;
    n->data = nullptr;
    n->size = 0;
    n->capacity = 0;
    n->first_child = nullptr;
    n->next_sibling = nullptr;
    return n;
}

static Node* find_child(Node* dir, const char* name) {
    for (Node* c = dir->first_child; c; c = c->next_sibling) {
        const char* a = c->name;
        const char* b = name;
        bool eq = true;
        while (*a && *b) { if (*a != *b) { eq = false; break; } a++; b++; }
        if (eq && *a == '\0' && *b == '\0') return c;
    }
    return nullptr;
}

// Walk/create the tree along a normalized path (no leading '/').
// create_dirs: make missing intermediate dirs (for O_CREATE).
static Node* walk(Node* base, const char* path, bool create_dirs) {
    Node* cur = base;
    const char* p = path;

    static char component[32];
    while (*p) {
        while (*p == '/') p++;
        if (*p == '\0') break;

        int i = 0;
        while (*p && *p != '/' && i < 31) component[i++] = *p++;
        component[i] = '\0';

        if (cur->is_dir == false) return nullptr;

        Node* next = find_child(cur, component);
        if (next == nullptr) {
            if (!create_dirs) return nullptr;
            // only create directories for intermediate components; whether
            // the FINAL component is a file or dir is decided by the caller
            bool last = (*p == '\0');
            next = new_node(component, !last);
            next->next_sibling = cur->first_child;
            cur->first_child = next;
        }
        cur = next;
    }
    return cur;
}

// --- ops implementation ---
// Handle = the Node* (offset/size tracking lives in VFS::FileHandle)

static void* ramfs_open(void* priv, const char* path, int flags, uint64_t* out_size) {
    (void)priv;
    Node* n = walk(root, path, (flags & VFS::O_CREATE) != 0);
    if (n == nullptr) return nullptr;
    if (n->is_dir) return nullptr; // open(2) on a dir: not supported
    if (flags & VFS::O_TRUNC) n->size = 0;
    *out_size = n->size;
    return n;
}

static uint64_t ramfs_read(void* priv, void* handle, void* buf,
                           uint64_t count, uint64_t offset) {
    (void)priv;
    Node* n = (Node*)handle;
    if (offset >= n->size) return 0;
    uint64_t avail = n->size - offset;
    uint64_t nread = (count < avail) ? count : avail;
    uint8_t* src = n->data + offset;
    uint8_t* dst = (uint8_t*)buf;
    for (uint64_t i = 0; i < nread; i++) dst[i] = src[i];
    return nread;
}

static uint64_t ramfs_write(void* priv, void* handle, const void* buf,
                            uint64_t count, uint64_t offset) {
    (void)priv;
    Node* n = (Node*)handle;
    uint64_t end = offset + count;

    // Grow the backing buffer if needed (kernel heap)
    if (end > n->capacity) {
        uint64_t newcap = (end + 4095) & ~(uint64_t)4095;
        uint8_t* nd = new uint8_t[newcap];
        for (uint64_t i = 0; i < n->size; i++) nd[i] = n->data[i];
        delete[] n->data;
        n->data = nd;
        n->capacity = newcap;
    }
    const uint8_t* src = (const uint8_t*)buf;
    for (uint64_t i = 0; i < count; i++) n->data[offset + i] = src[i];
    if (end > n->size) n->size = end;
    return count;
}

static void ramfs_close(void* priv, void* handle) {
    (void)priv; (void)handle; // nodes live in the tree; nothing to free
}

static uint64_t ramfs_readdir(void* priv, const char* path, char* buf, uint64_t max) {
    (void)priv;
    Node* n = walk(root, path, false);
    if (n == nullptr || n->is_dir == false) return 0;

    uint64_t written = 0;
    for (Node* c = n->first_child; c; c = c->next_sibling) {
        const char* name = c->name;
        while (*name && written + 3 < max) buf[written++] = *name++;
        if (c->is_dir && written + 3 < max) buf[written++] = '/';
        if (written + 3 < max) buf[written++] = '\n';
    }
    buf[written] = '\0';
    return written;
}

static bool ramfs_is_dir(void* priv, const char* path) {
    (void)priv;
    Node* n = walk(root, path, false);
    return n != nullptr && n->is_dir;
}

static int ramfs_mkdir(void* priv, const char* path) {
    (void)priv;
    // walk with creation of intermediate dirs, final node = dir
    Node* n = walk(root, path, true);
    if (n == nullptr) return -1;
    n->is_dir = true; // final component may have been created as a file
    return 0;
}

static int ramfs_truncate(void* priv, void* handle, uint64_t size) {
    (void)priv;
    Node* n = (Node*)handle;
    if (size > n->capacity) {
        uint64_t newcap = (size + 4095) & ~(uint64_t)4095;
        uint8_t* nd = new uint8_t[newcap];
        for (uint64_t i = 0; i < n->size; i++) nd[i] = n->data[i];
        for (uint64_t i = n->size; i < size; i++) nd[i] = 0; // zero-fill growth
        delete[] n->data;
        n->data = nd;
        n->capacity = newcap;
    } else if (size > n->size) {
        for (uint64_t i = n->size; i < size; i++) n->data[i] = 0;
    }
    n->size = size;
    return 0;
}

// Split a path into its parent directory node and final component name.
// parent_out needs 128 bytes, name_out 32. Returns the parent node (the root
// for a top-level name), or nullptr if the parent does not exist.
static Node* split_parent(const char* path, char* parent_out, char* name_out) {
    int i = 0;
    while (path[i]) i++;
    while (i > 0 && path[i - 1] == '/') i--;
    int slash = -1;
    for (int j = 0; j < i; j++) if (path[j] == '/') slash = j;

    int plen = (slash < 0) ? 0 : slash;
    for (int k = 0; k < plen && k < 127; k++) parent_out[k] = path[k];
    parent_out[plen] = '\0';

    const char* name = path + (slash < 0 ? 0 : slash + 1);
    int ni = 0;
    while (name[ni] && name[ni] != '/' && ni < 31) { name_out[ni] = name[ni]; ni++; }
    name_out[ni] = '\0';

    return walk(root, parent_out, false);
}

// Unlink a child from its parent's child list.
static void detach(Node* dir, Node* node) {
    Node* prev = nullptr;
    for (Node* c = dir->first_child; c; prev = c, c = c->next_sibling) {
        if (c != node) continue;
        if (prev) prev->next_sibling = c->next_sibling;
        else dir->first_child = c->next_sibling;
        return;
    }
}

static int ramfs_unlink(void* priv, const char* path) {
    (void)priv;
    static char parent[128], name[32];
    Node* dir = split_parent(path, parent, name);
    if (dir == nullptr || !dir->is_dir) return -1;

    Node* c = find_child(dir, name);
    if (c == nullptr) return -1;
    if (c->is_dir && c->first_child) return -1;   // non-empty dir

    detach(dir, c);
    if (c->data) delete[] c->data;
    delete c;
    return 0;
}

// Rename/move within the ramfs. Replaces an existing destination file; refuses
// to clobber a non-empty directory. (Moving a directory into its own subtree is
// not detected: nodes carry no parent pointer.)
static int ramfs_rename(void* priv, const char* from, const char* to) {
    (void)priv;
    static char from_parent[128], to_parent[128], from_name[32], to_name[32];
    Node* fdir = split_parent(from, from_parent, from_name);
    Node* tdir = split_parent(to, to_parent, to_name);
    if (fdir == nullptr || !fdir->is_dir) return -1;
    if (tdir == nullptr || !tdir->is_dir) return -1;
    if (from_name[0] == '\0' || to_name[0] == '\0') return -1;

    Node* src = find_child(fdir, from_name);
    if (src == nullptr) return -1;

    Node* dst = find_child(tdir, to_name);
    if (dst != nullptr && dst != src) {
        if (dst->is_dir && dst->first_child) return -1;   // non-empty dir
        detach(tdir, dst);
        if (dst->data) delete[] dst->data;
        delete dst;
    }

    detach(fdir, src);

    int i = 0;
    while (to_name[i] && i < 31) { src->name[i] = to_name[i]; i++; }
    src->name[i] = '\0';
    src->next_sibling = tdir->first_child;
    tdir->first_child = src;
    return 0;
}

// --- The filesystem instance ---
static VFS::Filesystem fs = {
    .fs_type  = "ramfs",
    .priv     = nullptr,
    .open     = ramfs_open,
    .read     = ramfs_read,
    .write    = ramfs_write,
    .close    = ramfs_close,
    .readdir  = ramfs_readdir,
    .is_dir   = ramfs_is_dir,
    .mkdir    = ramfs_mkdir,
    .truncate = ramfs_truncate,
    .unlink   = ramfs_unlink,
    .rename   = ramfs_rename,
};

VFS::Filesystem* instance() {
    return &fs;
}

void init() {
    root = new_node("", true);
    kprint("[RamFS] Ready.\n");
}

} // namespace RamFS
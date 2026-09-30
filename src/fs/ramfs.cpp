#include "ramfs.h"
#include "vfs.h"
#include "../kernel/heap.h"

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
        while (*name && written + 2 < max) buf[written++] = *name++;
        if (written + 2 < max) buf[written++] = '\n';
    }
    buf[written] = '\0';
    return written;
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
};

VFS::Filesystem* instance() {
    return &fs;
}

void init() {
    root = new_node("", true);
    kprint("[RamFS] Ready.\n");
}

} // namespace RamFS
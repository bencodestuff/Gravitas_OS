#include "fs/tar.h"
#include "fs/vfs.h"
#include "kernel/dev/modules.h"
#include "kernel/core/vmm.h"
#include "kernel/gfx/framebuffer.h"

extern "C" void kprint_uint(uint64_t value);

// --- ustar on-disk layout ---
// 512-byte header, then the file data, padded to a 512-byte boundary.
struct TarHeader {
    char name[100];     // 0
    char mode[8];       // 100
    char uid[8];        // 108
    char gid[8];        // 116
    char size[12];      // 124 (octal ASCII)
    char mtime[12];     // 136
    char chksum[8];     // 148
    char typeflag;      // 156 ('0' = regular file, '5' = directory)
    char linkname[100]; // 157
    char magic[6];      // 257 ("ustar")
    char version[2];    // 263
    char uname[32];     // 265
    char gname[32];     // 297
    char devmajor[8];   // 329
    char devminor[8];   // 337
    char prefix[155];   // 345
    char pad[12];       // 500 (padded to 512)
} __attribute__((packed));

static_assert(sizeof(TarHeader) == 512, "ustar header must be 512 bytes");

namespace Tar {

    static uint8_t* archive_base = nullptr;
    static uint64_t entry_count = 0;

    // --- tiny local string helpers (kernel has no libc) ---
    static bool str_eq(const char* a, const char* b) {
        while (*a && *b) { if (*a != *b) return false; a++; b++; }
        return *a == *b;
    }

    static uint64_t strlen_(const char* s) {
        uint64_t n = 0;
        while (s[n]) n++;
        return n;
    }

    static int strncmp_(const char* a, const char* b, uint64_t n) {
        for (uint64_t i = 0; i < n; i++) {
            if (a[i] != b[i]) return a[i] - b[i];
            if (a[i] == '\0') return 0;
        }
        return 0;
    }

    // Tar stores paths like "./hello.txt" (from `tar -C dir .`).
    // Strip a leading "./" so userland can just say "hello.txt".
    static const char* trim_dotslash(const char* s) {
        if (s[0] == '.' && s[1] == '/') return s + 2;
        return s;
    }

    static uint64_t octal_to_int(const char* s, int max_len) {
        uint64_t value = 0;
        for (int i = 0; i < max_len && s[i] != '\0'; i++) {
            if (s[i] < '0' || s[i] > '7') break;
            value = value * 8 + (uint64_t)(s[i] - '0');
        }
        return value;
    }

    static bool is_zero_block(const uint8_t* block) {
        for (int i = 0; i < 512; i++) {
            if (block[i] != 0) return false;
        }
        return true;
    }

    static bool is_ustar(const uint8_t* header) {
        const char* magic = "ustar";
        for (int i = 0; i < 5; i++) {
            if (header[257 + i] != (uint8_t)magic[i]) return false;
        }
        return true;
    }

    // The member starting at 'entry', or nullptr at the end of the archive
    // (a zero block, or something that is not a ustar header).
    static uint8_t* member_at(uint8_t* entry) {
        if (entry == nullptr || archive_base == nullptr) return nullptr;
        if (is_zero_block(entry) || !is_ustar(entry)) return nullptr;
        return entry;
    }

    static uint8_t* next_entry(uint8_t* entry) {
        uint64_t size = octal_to_int(((TarHeader*)entry)->size, 12);
        uint64_t padded = (size + 511) & ~(uint64_t)511;
        return entry + 512 + padded;
    }

    void init() {
        Modules::Module m;
        if (!Modules::find(".tar", &m)) {
            kprint("[Tar] No .tar module found!\n");
            return;
        }

        archive_base = (uint8_t*)m.data;

        // Count entries for the boot log (and sanity-check the walk).
        entry_count = 0;
        for (uint8_t* e = member_at(archive_base); e; e = member_at(next_entry(e)))
            entry_count++;

        kprint("[Tar] Mounted ");
        kprint(m.path);
        kprint(" (");
        kprint_uint(entry_count);
        kprint(" entries)\n");
    }

    bool is_mounted() {
        return archive_base != nullptr;
    }

    uint64_t count() {
        return entry_count;
    }

    // --- the flat archive, read by fs-relative path ---

    // The member at 'path', if any. Fills the data pointer, the size and the
    // member's ordinal (its position in the archive — a stable identity).
    static bool find_member(const char* path, const uint8_t** out_data,
                            uint64_t* out_size, uint32_t* out_ord) {
        uint32_t ord = 0;
        for (uint8_t* e = member_at(archive_base); e; e = member_at(next_entry(e)), ord++) {
            TarHeader* h = (TarHeader*)e;
            if (str_eq(trim_dotslash(h->name), path)) {
                if (out_data) *out_data = e + 512;
                if (out_size) *out_size = octal_to_int(h->size, 12);
                if (out_ord) *out_ord = ord;
                return true;
            }
        }
        return false;
    }

    // Is 'path' a directory: an explicit "path/" member, or any member living
    // inside it ("Pictures/Earth.bmp" implies "Pictures"). The ordinal of the
    // first such member becomes the directory's identity.
    static bool find_dir(const char* path, uint32_t* out_ord) {
        uint64_t plen = strlen_(path);
        uint32_t ord = 0;
        for (uint8_t* e = member_at(archive_base); e; e = member_at(next_entry(e)), ord++) {
            const char* m = trim_dotslash(((TarHeader*)e)->name);
            uint64_t mlen = strlen_(m);
            if (mlen == plen + 1 && m[plen] == '/' && strncmp_(m, path, plen) == 0) {
                if (out_ord) *out_ord = ord;
                return true;
            }
            if (mlen > plen + 1 && m[plen] == '/' && strncmp_(m, path, plen) == 0) {
                if (out_ord) *out_ord = ord;
                return true;
            }
        }
        return false;
    }

    // The first path component of member 'm' under directory 'dpath', with its
    // length and whether the component is a directory. nullptr when 'm' is not
    // under 'dpath' at all (or is the directory's own entry).
    static const char* child_component(const char* dpath, const char* m,
                                       uint64_t* clen, bool* isdir) {
        uint64_t plen = strlen_(dpath);
        uint64_t mlen = strlen_(m);
        if (mlen == 0) return nullptr;

        const char* rest;
        if (plen == 0) {
            rest = m;
        } else {
            if (mlen <= plen) return nullptr;
            if (strncmp_(m, dpath, plen) != 0 || m[plen] != '/') return nullptr;
            rest = m + plen + 1;
        }
        if (rest[0] == '\0') return nullptr;

        uint64_t rlen = strlen_(rest);
        bool dir = false;
        if (rest[rlen - 1] == '/') { dir = true; rlen--; }
        uint64_t nlen = rlen;
        for (uint64_t i = 0; i < rlen; i++) {
            if (rest[i] == '/') { nlen = i; dir = true; break; }
        }
        *clen = nlen;
        *isdir = dir;
        return rest;
    }

    // --- VFS nodes over the archive ---
    // A tar node is a window: the archive is immutable, so a node is just a
    // path plus, for a file, a pointer to the member's bytes (zero-copy).

    struct TarNode {
        char path[256];
        bool is_dir;
        uint64_t size;
        const uint8_t* data;
    };

    static void join_path(const char* dir, const char* name, char* out, uint64_t max) {
        uint64_t n = 0;
        if (dir[0] != '\0') {
            for (uint64_t i = 0; dir[i] && n + 1 < max; i++) out[n++] = dir[i];
            if (n + 1 < max) out[n++] = '/';
        }
        for (uint64_t i = 0; name[i] && n + 1 < max; i++) out[n++] = name[i];
        out[n] = '\0';
    }

    static VFS::Node* tar_root(void* priv);
    static VFS::Node* tar_lookup(void* priv, VFS::Node* dir, const char* name);
    static bool tar_readdir(void* priv, VFS::Node* dir, uint64_t index, VFS::DirEntry* out);
    static uint64_t tar_read(void* priv, VFS::Node* node, void* buf, uint64_t count, uint64_t offset);
    static void tar_release(void* priv, VFS::Node* node);
    static int tar_statfs(void* priv, VFS::StatFs* out);
    static int tar_map(void* priv, VFS::Node* node, uint64_t* out_phys, uint64_t* out_size);

    static VFS::Filesystem tarfs = {
        .fs_type  = "tarfs",
        .priv     = nullptr,
        .root     = tar_root,
        .lookup   = tar_lookup,
        .readdir  = tar_readdir,
        .read      = tar_read,
        .write     = nullptr,   // read-only
        .truncate = nullptr,   // read-only
        .create   = nullptr,   // read-only
        .unlink   = nullptr,   // read-only
        .rename   = nullptr,   // read-only
        .release  = tar_release,
        .statfs   = tar_statfs,
        .map      = tar_map,   // zero-copy: the image is immutable
    };

    static VFS::Node* make_node(const char* path, bool is_dir, uint64_t size,
                                const uint8_t* data, uint32_t ino) {
        VFS::Node* n = VFS::node_new(&tarfs, is_dir ? VFS::FT_DIR : VFS::FT_FILE);
        if (n == nullptr) return nullptr;
        TarNode* tn = new TarNode;
        if (tn == nullptr) { delete n; return nullptr; }

        uint64_t i = 0;
        while (path[i] && i < sizeof(tn->path) - 1) { tn->path[i] = path[i]; i++; }
        tn->path[i] = '\0';
        tn->is_dir = is_dir;
        tn->size = size;
        tn->data = data;

        n->priv = tn;
        n->size = size;
        n->ino = ino;
        n->mode = is_dir ? 0555 : 0444;   // the image is read-only
        n->mtime = 0;                     // the archive carries one; not modelled
        return n;
    }

    static VFS::Node* tar_root(void* priv) {
        (void)priv;
        return make_node("", true, 0, nullptr, 1);
    }

    static VFS::Node* tar_lookup(void* priv, VFS::Node* dir, const char* name) {
        (void)priv;
        TarNode* d = (TarNode*)dir->priv;
        if (!d->is_dir || name[0] == '\0') return nullptr;

        char path[256];
        join_path(d->path, name, path, sizeof(path));
        if (path[0] == '\0') return nullptr;

        const uint8_t* data = nullptr;
        uint64_t size = 0;
        uint32_t ord = 0;
        if (find_member(path, &data, &size, &ord))
            return make_node(path, false, size, data, ord + 1);
        if (find_dir(path, &ord))
            return make_node(path, true, 0, nullptr, ord + 1);
        return nullptr;
    }

    // Has an entry *before* 'stop' already produced this child? Directories in
    // a tar are implied by their members, so the same child can appear many
    // times and must be reported once.
    static bool child_seen_before(const char* dpath, const char* comp, uint64_t clen,
                                  uint8_t* stop) {
        for (uint8_t* e = member_at(archive_base); e && e != stop; e = member_at(next_entry(e))) {
            uint64_t c2len = 0;
            bool d2 = false;
            const char* r2 = child_component(dpath, trim_dotslash(((TarHeader*)e)->name),
                                             &c2len, &d2);
            if (r2 && c2len == clen && strncmp_(r2, comp, clen) == 0) return true;
        }
        return false;
    }

    static bool tar_readdir(void* priv, VFS::Node* dir, uint64_t index, VFS::DirEntry* out) {
        (void)priv;
        TarNode* d = (TarNode*)dir->priv;
        if (!d->is_dir) return false;

        uint64_t seen = 0;
        uint32_t ord = 0;
        for (uint8_t* e = member_at(archive_base); e; e = member_at(next_entry(e)), ord++) {
            const char* m = trim_dotslash(((TarHeader*)e)->name);
            uint64_t clen = 0;
            bool isdir = false;
            const char* rest = child_component(d->path, m, &clen, &isdir);
            if (rest == nullptr) continue;
            if (child_seen_before(d->path, rest, clen, e)) continue;

            if (seen == index) {
                uint64_t n = 0;
                while (n < clen && n + 1 < sizeof(out->name)) { out->name[n] = rest[n]; n++; }
                out->name[n] = '\0';
                out->type = isdir ? VFS::FT_DIR : VFS::FT_FILE;
                out->ino = ord + 1;
                return true;
            }
            seen++;
        }
        return false;
    }

    static uint64_t tar_read(void* priv, VFS::Node* node, void* buf,
                             uint64_t count, uint64_t offset) {
        (void)priv;
        TarNode* tn = (TarNode*)node->priv;
        if (tn->data == nullptr) return 0;
        const uint8_t* src = tn->data + offset;
        uint8_t* dst = (uint8_t*)buf;
        for (uint64_t i = 0; i < count; i++) dst[i] = src[i];
        return count;
    }

    static void tar_release(void* priv, VFS::Node* node) {
        (void)priv;
        delete (TarNode*)node->priv;
        delete node;
    }

    // An image is not a medium with free space: it is read-only, full stop.
    static int tar_statfs(void* priv, VFS::StatFs* out) {
        (void)priv;
        out->writable = false;
        out->free = 0;
        return 0;
    }

    // tarfs members are slices of the immutable image, so a mapping is just the
    // physical address of their bytes: no copy, and they can never move.
    static int tar_map(void* priv, VFS::Node* node, uint64_t* out_phys,
                       uint64_t* out_size) {
        (void)priv;
        TarNode* tn = (TarNode*)node->priv;
        if (tn->data == nullptr || tn->size == 0) return -1;
        *out_phys = VMM::phys_of((void*)tn->data);
        *out_size = tn->size;
        return 0;
    }

    VFS::Filesystem* vfs_instance() {
        return &tarfs;
    }
}

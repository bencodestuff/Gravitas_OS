#include <limine.h>
#include "fs/tar.h"
#include "fs/vfs.h"
#include "kernel/gfx/framebuffer.h"

// Defined in initrd.cpp
extern volatile struct limine_module_request module_request;

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

    static bool str_ends_with(const char* s, const char* suffix) {
        const char* p = s;
        while (*p) p++;
        const char* q = suffix;
        while (*q) q++;
        if (q - suffix > p - s) return false;
        while (q > suffix) { if (*--p != *--q) return false; }
        return true;
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

    // Advance '*cursor' to the next header. Returns false at end of archive.
    static bool next_entry(uint8_t*& cursor, TarHeader*& header,
                           uint8_t*& data, uint64_t& size) {
        if (archive_base == nullptr || cursor == nullptr) return false;

        // End of archive: two consecutive zero blocks. One is enough for us,
        // since we never walk past it anyway.
        if (is_zero_block(cursor) || !is_ustar(cursor)) return false;

        header = (TarHeader*)cursor;
        size = octal_to_int(header->size, 12);
        data = cursor + 512;

        // Advance past data, padded out to a 512-byte boundary.
        uint64_t padded = (size + 511) & ~(uint64_t)511;
        cursor += 512 + padded;
        return true;
    }

    void init() {
        if (module_request.response == nullptr) {
            kprint("[Tar] No modules loaded.\n");
            return;
        }

        for (uint64_t i = 0; i < module_request.response->module_count; i++) {
            struct limine_file* file = module_request.response->modules[i];
            if (file->path != nullptr && str_ends_with(file->path, ".tar")) {
                archive_base = (uint8_t*)file->address;

                // Count entries for the boot log (and sanity-check the walk).
                entry_count = 0;
                uint8_t* cursor = archive_base;
                TarHeader* h; uint8_t* d; uint64_t sz;
                while (next_entry(cursor, h, d, sz)) entry_count++;

                kprint("[Tar] Mounted ");
                kprint(file->path);
                kprint(" (");
                kprint_uint(entry_count);
                kprint(" entries)\n");
                return;
            }
        }
        kprint("[Tar] No .tar module found!\n");
    }

    bool is_mounted() {
        return archive_base != nullptr;
    }

    uint64_t count() {
        return entry_count;
    }

    void list_into(char* buf, uint64_t max) {
        if (buf == nullptr || max == 0) return;
        uint64_t written = 0;

        uint8_t* cursor = archive_base;
        TarHeader* h; uint8_t* d; uint64_t sz;
        while (next_entry(cursor, h, d, sz)) {
            const char* name = trim_dotslash(h->name);
            // Copy "name\n", respecting the buffer limit
            while (*name && written + 2 < max) buf[written++] = *name++;
            if (written + 2 < max) buf[written++] = '\n';
        }
        buf[written] = '\0';
    }

    bool read_file(const char* path, char** out_data, uint64_t* out_size) {
        if (archive_base == nullptr || path == nullptr) return false;

        uint8_t* cursor = archive_base;
        TarHeader* h; uint8_t* d; uint64_t sz;
        while (next_entry(cursor, h, d, sz)) {
            if (str_eq(trim_dotslash(h->name), path)) {
                if (out_data) *out_data = (char*)d;
                if (out_size) *out_size = sz;
                return true;
            }
        }
        return false;
    }

    // --- VFS adapter: read-only Filesystem over the mounted archive ---
    // Handle = pointer to the member data inside the archive (zero-copy).
    // Offsets are tracked in the VFS FileHandle; the size was reported at
    // open, and the VFS clamps reads against it.

    static void* tarfs_open(void* priv, const char* path, int flags, uint64_t* out_size) {
        (void)priv;
        if (flags & (VFS::O_CREATE | VFS::O_WRONLY | VFS::O_RDWR)) return nullptr; // RO fs
        char* data = nullptr;
        uint64_t size = 0;
        if (!read_file(path, &data, &size)) return nullptr;
        *out_size = size;
        return data;
    }

    static uint64_t tarfs_read(void* priv, void* handle, void* buf,
                               uint64_t count, uint64_t offset) {
        (void)priv;
        uint8_t* src = (uint8_t*)handle + offset;
        uint8_t* dst = (uint8_t*)buf;
        for (uint64_t i = 0; i < count; i++) dst[i] = src[i];
        return count;
    }

    static void tarfs_close(void* priv, void* handle) {
        (void)priv; (void)handle; // data lives inside the archive
    }

    static uint64_t strlen_(const char* s) {
        uint64_t n = 0;
        while (s[n]) n++;
        return n;
    }
    static bool strncmp_(const char* a, const char* b, uint64_t n) {
        for (uint64_t i = 0; i < n; i++) {
            if (a[i] != b[i]) return a[i] - b[i];
            if (a[i] == '\0') return 0;
        }
        return 0;
    }

    static uint64_t tarfs_readdir(void* priv, const char* path, char* buf, uint64_t max) {
        (void)priv;
        uint64_t plen = strlen_(path);
        uint8_t* cursor = archive_base;
        TarHeader* h; uint8_t* d; uint64_t sz;

        // Collect direct children (deduped), subdirectories marked '/'
        static char children[64][40];
        bool child_is_dir[64];
        int nchild = 0;

        while (next_entry(cursor, h, d, sz)) {
            const char* m = trim_dotslash(h->name);
            uint64_t mlen = strlen_(m);
            if (mlen == 0) continue;

            // What part of this member is under 'path'?
            const char* rest = nullptr;
            if (path[0] == '\0') {
                rest = m;                       // root: members are children
            } else {
                if (mlen <= plen) continue;
                if (strncmp_(m, path, plen) != 0 || m[plen] != '/') continue;
                rest = m + plen + 1;            // inside this dir
            }
            if (rest[0] == '\0') continue;      // the dir's own entry

            bool isdir = false;
            uint64_t rlen = strlen_(rest);
            if (rest[rlen-1] == '/') { isdir = true; rlen--; }

            // First component only; anything below it means this child
            // is itself a directory
            uint64_t nlen = rlen;
            for (uint64_t i = 0; i < rlen; i++) {
                if (rest[i] == '/') { nlen = i; isdir = true; break; }
            }
            if (nlen >= 39) nlen = 39;

            // dedupe (one dir can be implied by many members)
            bool dup = false;
            for (int i = 0; i < nchild; i++) {
                if (strncmp_(children[i], rest, nlen) == 0 && children[i][nlen] == '\0') {
                    dup = true; break;
                }
            }
            if (dup || nchild >= 64) continue;

            for (uint64_t i = 0; i < nlen; i++) children[nchild][i] = rest[i];
            children[nchild][nlen] = '\0';
            child_is_dir[nchild] = isdir;
            nchild++;
        }

        // Emit: one name per line, dirs with trailing '/'
        uint64_t written = 0;
        for (int i = 0; i < nchild; i++) {
            const char* n = children[i];
            while (*n && written + 3 < max) buf[written++] = *n++;
            if (child_is_dir[i] && written + 3 < max) buf[written++] = '/';
            if (written + 3 < max) buf[written++] = '\n';
        }
        buf[written] = '\0';
        return written;
    }

    static bool tarfs_is_dir(void* priv, const char* path) {
        (void)priv;
        if (path[0] == '\0') return true; // the archive root
        // A path is a directory if any member lives "inside" it:
        // member names like "Pictures/Earth.bmp" imply dir "Pictures"
        uint64_t plen = strlen_(path);
        uint8_t* cursor = archive_base;
        TarHeader* h; uint8_t* d; uint64_t sz;
        while (next_entry(cursor, h, d, sz)) {
            const char* m = trim_dotslash(h->name);
            uint64_t mlen = strlen_(m);
            // explicit dir entry "Pictures/"?
            if (mlen == plen + 1 && m[plen] == '/' &&
                strncmp_(m, path, plen) == 0) return true;
            // implicit dir: member "Pictures/x" under "Pictures"?
            if (mlen > plen + 1 && m[plen] == '/' &&
                strncmp_(m, path, plen) == 0) return true;
        }
        return false;
    }

    static VFS::Filesystem tarfs = {
        .fs_type  = "tarfs",
        .priv     = nullptr,
        .open     = tarfs_open,
        .read     = tarfs_read,
        .write    = nullptr,   // read-only
        .close    = tarfs_close,
        .readdir  = tarfs_readdir,
        .is_dir   = tarfs_is_dir,
        .mkdir    = nullptr,   // read-only
    };

    VFS::Filesystem* vfs_instance() {
        return &tarfs;
    }
}

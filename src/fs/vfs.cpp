#include "vfs.h"
#include "../drivers/framebuffer.h"
#include "../kernel/heap.h"
#include "../kernel/sched.h"

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
    // components stack: offsets into out of each component's start
    static uint64_t starts[32];
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

Filesystem* resolve(const char* path, const char** rel_path) {
    static char canon[256];
    normalize(path, canon, sizeof(canon));

    Mount* m = find_mount(canon);
    if (m == nullptr) return nullptr;

    uint64_t ml = 0;
    while (m->path[ml]) ml++;
    *rel_path = canon + ml;         // "/bin/ls.elf" -> "/ls.elf"
    if (**rel_path == '/') (*rel_path)++;  // -> "ls.elf"; "/" -> ""
    return m->fs;
}

bool mount(const char* path, Filesystem* fs) {
    if (mount_count >= MAX_MOUNTS) return false;
    static char canon_storage[MAX_MOUNTS][128];
    normalize(path, canon_storage[mount_count], 128);
    mounts[mount_count].path = canon_storage[mount_count];
    mounts[mount_count].fs = fs;
    mount_count++;
    return true;
}

// --- Bare-name fallback (the embryonic PATH) ---
// "ls.elf" with no slash: try /bin first (that's where programs live),
// then the bare name itself.
static bool resolve_path_fallback(const char* path, char* out, uint64_t out_max,
                                  const char** effective) {
    bool has_slash = false;
    for (const char* p = path; *p; p++) {
        if (*p == '/') { has_slash = true; break; }
    }
    if (has_slash) {
        *effective = path;
        return true;
    }
    // /bin/<name>
    uint64_t i = 0;
    out[i++] = '/';
    out[i++] = 'b';
    out[i++] = 'i';
    out[i++] = 'n';
    out[i++] = '/';
    while (*path && i + 1 < out_max) out[i++] = *path++;
    out[i] = '\0';
    *effective = out;
    return true;
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
    for (int i = 0; i < MAX_FDS; i++) {
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
int open_fd(const char* path, int flags) {
    static char fixed[256];
    const char* effective = nullptr;
    resolve_path_fallback(path, fixed, sizeof(fixed), &effective);

    const char* rel = nullptr;
    Filesystem* fs = resolve(effective, &rel);
    if (fs == nullptr || fs->open == nullptr) return -1;

    uint64_t size = 0;
    void* h = fs->open(fs->priv, rel, flags, &size);
    if (h == nullptr) return -1;

    int fd = fd_slot_alloc(fs, h, size);
    if (fd == -1) {
        fs->close(fs->priv, h); // don't leak the backend handle
    }
    return fd;
}

long read_fd(int fd, void* buf, uint64_t count) {
    FileHandle* h = fd_slot_get(fd);
    if (h == nullptr || h->fs->read == nullptr) return -1;
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
    if (h == nullptr || h->fs->write == nullptr) return -1;
    uint64_t n = h->fs->write(h->fs->priv, h->fs_handle, buf, count, h->offset);
    h->offset += n;
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

// --- Whole-file read (spawn + kernel use) ---
bool read_full(const char* path, char* buf, uint64_t max, uint64_t* out_size) {
    static char fixed[256];
    const char* effective = nullptr;
    resolve_path_fallback(path, fixed, sizeof(fixed), &effective);

    const char* rel = nullptr;
    Filesystem* fs = resolve(effective, &rel);
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
    static char fixed[256];
    const char* effective = nullptr;
    resolve_path_fallback(path, fixed, sizeof(fixed), &effective);

    const char* rel = nullptr;
    Filesystem* fs = resolve(effective, &rel);
    if (fs == nullptr || fs->readdir == nullptr) return 0;
    return fs->readdir(fs->priv, rel, buf, max);
}

// --- fd size accessor (spawn uses it to size the ELF buffer) ---
uint64_t fd_size(int fd) {
    FileHandle* h = fd_slot_get(fd);
    return (h == nullptr) ? 0 : h->size;
}

// --- fd slot lookup used by syscalls ---
FileHandle* fd_get(int fd) {
    return fd_slot_get(fd);
}

} // namespace VFS
#pragma once
#include <stdint.h>

// --- Virtual filesystem layer ---
// Design: filesystems are self-contained backends registered into a mount
// table. Every backend implements the `Filesystem` ops table (a C-style
// vtable) and owns its private data. The VFS resolves paths (longest
// matching mount prefix), maintains NO per-file state itself, and hands
// out integer fds backed by per-process handle tables.
//
// Adding a filesystem later = write one Filesystem struct + one mount()
// call. Nothing else in the kernel changes.

namespace VFS {

constexpr int MAX_FDS     = 16;
constexpr int MAX_MOUNTS  = 8;
constexpr int MAX_HANDLES = 64;   // open files kernel-wide

// Open flags (subset of Unix semantics for now)
constexpr int O_RDONLY   = 0;
constexpr int O_WRONLY   = 1;
constexpr int O_RDWR     = 2;
constexpr int O_CREATE   = 4;     // create if missing (ramfs only)

// --- Filesystem backend interface ---
struct Filesystem {
    const char* fs_type;          // "ramfs", "tarfs" — debug/logging only
    void* priv;                   // backend instance data

    // Open 'path' inside this fs. Returns a backend handle (opaque),
    // sets *out_size to the file's byte length, or nullptr if missing.
    void* (*open)(void* priv, const char* path, int flags, uint64_t* out_size);

    // Read up to 'count' bytes at 'offset' into 'buf'. Returns bytes read.
    uint64_t (*read)(void* priv, void* handle, void* buf,
                     uint64_t count, uint64_t offset);

    // Write 'count' bytes at 'offset'. Returns bytes written (0 = unsupported).
    uint64_t (*write)(void* priv, void* handle, const void* buf,
                      uint64_t count, uint64_t offset);

    // Release a backend handle
    void (*close)(void* priv, void* handle);

    // Fill 'buf' with entries of the directory at 'path', one name per
    // line, NUL-terminated. Directory names get a trailing '/'.
    // Returns 0 if path is not a directory.
    uint64_t (*readdir)(void* priv, const char* path, char* buf, uint64_t max);

    // Is this path an existing directory?
    bool (*is_dir)(void* priv, const char* path);

    // Create a directory (ramfs only; used by mount() for mountpoints)
    int (*mkdir)(void* priv, const char* path);
};

// --- An open file (referenced by an fd) ---
struct FileHandle {
    Filesystem* fs;
    void*       fs_handle;   // backend's opaque handle
    uint64_t    offset;
    uint64_t    size;
};

// --- A filesystem instance rooted at a path (e.g. "/bin") ---
struct Mount {
    const char* path;
    Filesystem* fs;
};

// --- Kernel API ---

// Mount the root ramfs; called once at boot
void init();

// Register a filesystem at a mount point (e.g. "/bin")
bool mount(const char* path, Filesystem* fs);

// fd operations (fd tables live in the current task)
int  open_fd(const char* path, int flags);            // fd or -1
long read_fd(int fd, void* buf, uint64_t count);      // bytes or -1
long write_fd(int fd, const void* buf, uint64_t count); // bytes or -1
void close_fd(int fd);
void close_fd_for(FileHandle** fds, int fd);          // arbitrary table (task exit)

// Resolve a path to (filesystem, fs-relative path). Longest mount-prefix
// wins. Relative paths resolve against the current task's CWD.
// Returns nullptr if nothing covers the path.
Filesystem* resolve(const char* path, const char** rel_path);

// Is this path an existing directory? (relative paths resolve vs CWD)
bool is_directory(const char* path);

// Canonicalize a path: CWD-relative join, '.'/'..' resolution, absolute.
// Used by chdir to store the real working directory.
void canonicalize(const char* path, char* out, uint64_t out_max);

// Convenience: read a whole file into a buffer (kernel-side; used by spawn)
bool read_full(const char* path, char* buf, uint64_t max, uint64_t* out_size);

// Directory listing into a newline-separated, NUL-terminated buffer
uint64_t readdir(const char* path, char* buf, uint64_t max);

// fd table plumbing (implemented against the current task in sched.cpp)
void      fds_init_current();               // zero the table
FileHandle** fds_of_current();               // the current task's table
FileHandle*  fd_slot_get(int fd);
uint64_t     fd_size(int fd);                // bytes reported at open
int          fd_slot_alloc(Filesystem* fs, void* fs_handle, uint64_t size);
}
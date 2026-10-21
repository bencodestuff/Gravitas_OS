#pragma once
#include <stdint.h>

// --- Virtual filesystem layer ---
// Design: filesystems are self-contained backends registered into a mount
// table. Everything above this line works in *nodes*, not path strings: a node
// is one file or directory with an identity, a type and its metadata. Path
// walking happens in one place (here); a backend answers per-node questions —
// what is this name, what is the i-th entry, read this, write this — and owns
// its private data. The VFS keeps no per-file state of its own and hands out
// integer fds backed by per-process handle tables.
//
// Adding a filesystem later = write one Filesystem struct + one mount() call.
// Nothing else in the kernel changes.

namespace VFS {

constexpr int MAX_FDS     = 16;
constexpr int MAX_MOUNTS  = 8;
constexpr int MAX_HANDLES = 64;   // open files kernel-wide

// The longest absolute path the VFS will carry. It has to be comfortably more
// than NAME_MAX: a maximal 255-byte name still needs room for the directory it
// lives in, and a path that does not fit is refused rather than cut down.
// (The CWD is separate and much smaller — see the scheduler.)
constexpr int PATH_MAX = 512;

// The longest single name any backend may hand back (and the ABI's ceiling).
// ext2 permits 255 bytes, so 256 is the smallest value that can hold one: a
// smaller cap would silently truncate a real filesystem's names, and a name that
// lists but cannot be opened is worse than a refusal.
constexpr int NAME_MAX = 256;

// The most a single write may ask a backend to take. No file this kernel can
// hold comes close (the ramfs cap is 1 MB), while a count past this is not a
// real buffer: the kernel would have to read that many bytes from user memory.
// The VFS refuses it before a backend ever has to defend its own arithmetic.
constexpr uint64_t MAX_WRITE_BYTES = 16ull << 20; // 16 MB

// Descriptors 0, 1 and 2 are reserved for the standard streams. The kernel has
// no console file object — userland maps those three to the console — but
// reserving the slots keeps VFS descriptors from colliding with them, which is
// what POSIX code expects (the first open() is 3).
constexpr int FD_STD_STREAMS = 3;

// Open flags (subset of Unix semantics for now)
constexpr int O_RDONLY   = 0;
constexpr int O_WRONLY   = 1;
constexpr int O_RDWR     = 2;
constexpr int O_CREATE   = 4;     // create if missing
constexpr int O_TRUNC    = 8;     // truncate to 0 on open
constexpr int O_APPEND   = 0x10;  // every write lands at the current end
constexpr int O_EXCL     = 0x20;  // with O_CREATE: fail if it already exists

enum FileType : uint8_t {
    FT_FILE = 0,
    FT_DIR  = 1,
};

struct Filesystem;

// --- A node: one file or directory, as its filesystem sees it ---
// Allocated by the backend (node_new) and disposed by that backend's release().
// The VFS reads the fields; 'priv' belongs to the backend, which is also what
// decides what a node *is* — a ramfs node is refcounted so it survives an
// unlink while open, a tarfs node is a window onto the immutable image.
struct Node {
    Filesystem* fs;
    void*       priv;
    FileType    type;
    uint64_t    size;    // bytes (a file); 0 for a directory
    uint32_t    ino;     // stable identity within the filesystem; 0 = none
    uint32_t    mode;    // permission bits (9); 0 = the filesystem's default
    uint64_t    mtime;   // seconds since the epoch; 0 = unknown
};

// One directory entry, as readdir reports it.
struct DirEntry {
    char     name[NAME_MAX];
    FileType type;
    uint32_t ino;
};

// A file's metadata, without opening it.
struct Stat {
    uint64_t size;
    FileType type;
    uint32_t ino;
    uint32_t mode;    // permission bits; the type is in 'type'
    uint64_t mtime;   // seconds since the epoch; 0 = unknown
};

// How much room a filesystem has.
struct StatFs {
    uint64_t total;      // bytes the medium holds; 0 = unknown
    uint64_t free;       // bytes that can still be written
    bool     writable;   // false when nothing can be written here at all
    const char* type;    // the backend's name (its fs_type)
};

// --- Filesystem backend interface ---
struct Filesystem {
    const char* fs_type;          // "ramfs", "tarfs", "ext2" — logging only
    void* priv;                   // backend instance data

    // The root directory of this filesystem. Never null for a mounted fs.
    Node* (*root)(void* priv);

    // Look up one component inside directory 'dir'. Returns a node the caller
    // releases, or nullptr when there is no such entry.
    Node* (*lookup)(void* priv, Node* dir, const char* name);

    // The i-th entry of directory 'dir' (0-based). False past the end.
    bool (*readdir)(void* priv, Node* dir, uint64_t index, DirEntry* out);

    // Read a file's bytes: returns how many were read. Write: returns how many
    // were written (0 for a real request means out of room — the VFS reports
    // ENOSPC; a read-only backend has no write at all).
    uint64_t (*read)(void* priv, Node* node, void* buf,
                     uint64_t count, uint64_t offset);
    uint64_t (*write)(void* priv, Node* node, const void* buf,
                      uint64_t count, uint64_t offset);

    // Set a file's length (nullptr if the backend cannot). Shrinking discards;
    // growing zero-fills. Returns 0 or -1.
    int (*truncate)(void* priv, Node* node, uint64_t size);

    // Create an entry in 'dir'. Fills *out with a node the caller releases.
    // nullptr when the filesystem is read-only. Returns 0 or -1.
    int (*create)(void* priv, Node* dir, const char* name, FileType type,
                  Node** out);

    // Remove an entry from 'dir' (nullptr when read-only). Returns 0 or -1.
    int (*unlink)(void* priv, Node* dir, const char* name);

    // Move 'from' in 'from_dir' to 'to' in 'to_dir' (nullptr when read-only).
    // Both directories belong to this filesystem. Returns 0 or -1.
    int (*rename)(void* priv, Node* from_dir, const char* from,
                  Node* to_dir, const char* to);

    // Dispose a node and whatever the backend holds for it.
    void (*release)(void* priv, Node* node);

    // How much room this filesystem has: 'total' is the medium, 'free' is what
    // can still be written (so a read-only filesystem reports 0, whatever the
    // medium holds), and 'writable' says whether anything can be written at all.
    // nullptr when the backend cannot say, and then the VFS reports what it can
    // infer. Returns 0 or -1.
    int (*statfs)(void* priv, StatFs* out);

    // The physical address and length of a file's bytes, for a zero-copy
    // mapping. Only a backend whose bytes cannot move can promise that (an
    // immutable image can; a buffer that may be reallocated cannot), so it is
    // nullptr elsewhere. Returns 0 or -1.
    int (*map)(void* priv, Node* node, uint64_t* out_phys, uint64_t* out_size);
};

// --- An open file (referenced by an fd) ---
// A descriptor is one of three things, tagged by 'kind':
//   File    — a node in a filesystem, with a read/write position
//   Console — the caller's console endpoint (the standard streams): reads take
//             the keyboard, writes go to the console (or to the terminal that
//             hosts this task). The endpoint is chosen per task, so the handle
//             carries no state of its own
//   Pipe    — one end of a byte pipe (pipe_write says which end)
//
// 'refs' counts the fd slots (in this task and any others) that name this
// handle. dup2 and spawn inheritance *share* a handle rather than copy it, so a
// file's offset and a pipe end's open/closed state mean the same thing to every
// holder — POSIX's "open file description". The handle dies when the last
// reference closes.
enum class FdKind : uint8_t {
    File    = 0,
    Console = 1,
    Pipe    = 2,
};

struct Pipe;   // the pipe pool lives in vfs.cpp

struct FileHandle {
    FdKind   kind       = FdKind::File;
    uint32_t refs       = 0;      // fd slots naming this handle
    Node*    node       = nullptr;// File: the node (one reference while refs > 0)
    uint64_t offset     = 0;      // File: read/write position
    bool     append     = false;  // File: writes always land at the current end
    Pipe*    pipe       = nullptr;// Pipe: the shared buffer
    bool     pipe_write = false;  // Pipe: true = the write end
};

// --- A filesystem instance rooted at a path (e.g. "/bin") ---
struct Mount {
    const char* path;
    Filesystem* fs;
};

// --- Node helpers, for backends and the VFS alike ---

// A zeroed node of 'type' belonging to 'fs'. The caller fills in priv and the
// metadata, and hands the node back to its release() when done.
Node* node_new(Filesystem* fs, FileType type);

// Walk a filesystem-relative path ("" = the root) to its node. The caller
// releases it. Returns nullptr if any component is missing.
Node* node_walk(Filesystem* fs, const char* rel);

// Walk a path, creating missing components — the last one as 'final_type', any
// earlier ones as directories. Returns the final node (the caller releases) or
// nullptr. This is what makes `open("a/b.txt", O_CREATE)` land in a real place
// rather than nowhere.
Node* node_create(Filesystem* fs, const char* rel, FileType final_type);

// Split a filesystem-relative path into the parent's path and the final name.
// False when there is no final name ("" or "/"), or when the name does not fit
// NAME_MAX. 'parent' holds PATH_MAX bytes, 'name' holds NAME_MAX.
bool node_split(const char* rel, char* parent, char* name);

// --- Kernel API ---

// Mount the root ramfs; called once at boot
void init();

// Register a filesystem at a mount point (e.g. "/bin")
bool mount(const char* path, Filesystem* fs);

// fd operations (fd tables live in the current task).
// Failures return a negated errno (see kernel/core/errno.h): ENOENT, EISDIR,
// EMFILE, EBADF, EACCES, EROFS, EINVAL, EIO.
int  open_fd(const char* path, int flags);            // fd or -errno
long read_fd(int fd, void* buf, uint64_t count);      // bytes or -errno
long write_fd(int fd, const void* buf, uint64_t count); // bytes or -errno
void close_fd(int fd);
void close_fd_for(FileHandle** fds, int fd);          // arbitrary table (task exit)

// Duplicate a descriptor. dup_fd() returns a new descriptor (never 0-2) sharing
// fd's open file description; dup2_fd() points newfd at oldfd's, closing
// whatever newfd held. Both share, so a later lseek or close is seen by every
// holder. Return the new fd, or -errno.
int dup_fd(int fd);
int dup2_fd(int oldfd, int newfd);

// Create a pipe: out[0] is the read end, out[1] the write end. A reader blocks
// until a writer sends (or the last writer closes, which is EOF); a writer
// blocks when the buffer is full. Returns 0 or -errno.
int pipe_fd(int out[2]);

// Forget a task that is exiting or being killed: drop its id from every pipe's
// reader and writer wait lists. A dead task can never be woken, and the lists
// hold only MAX_PIPE_WAITERS ids, so leaving it there would slowly wedge a pipe.
void forget_task(int task_id);

// Copy the current task's descriptor table into 'dst' (a newborn task's),
// sharing every handle. Console entries are the shared console endpoint. This
// is what makes redirection survive a spawn: the child writes where the parent
// pointed fd 1.
void fds_inherit(FileHandle** dst);

// Path rule: lookups (open/stat) accept a bare name — the CWD is tried
// first, then /bin. Creation and removal (mkdir/unlink) never search: they
// resolve against the CWD (or an absolute path) only.

// Is this path an existing directory? (relative paths resolve vs CWD)
bool is_directory(const char* path);

// Canonicalize a path: CWD-relative join, '.'/'..' resolution, absolute.
// Used by chdir to store the real working directory. False when the path cannot
// be represented (see normalize).
bool canonicalize(const char* path, char* out, uint64_t out_max);

// Convenience: read a whole file into a buffer (kernel-side; used by spawn)
bool read_full(const char* path, char* buf, uint64_t max, uint64_t* out_size);

// Directory entries of 'path' into 'out[0..max)'. Returns the number written,
// or a negated errno (ENOENT, ENOTDIR). Typed: a name, a type and an inode.
long readdir_entries(const char* path, DirEntry* out, uint64_t max);

// Seek an fd's read/write offset. 'whence' is 0=SET, 1=CUR, 2=END. Returns
// the new offset, or -errno (EBADF/EINVAL).
long lseek_fd(int fd, long offset, int whence);

// Create a directory / remove a file, by path. Return 0 or -errno
// (ENOENT/EROFS/EIO).
int mkdir(const char* path);
int unlink(const char* path);

// Rename/move a path. Both sides must live on the same filesystem. Returns 0
// or -errno (ENOENT/EINVAL/EROFS).
int rename(const char* from, const char* to);

// Set a file's length by fd. Returns 0 or -errno (EBADF/EROFS/EIO).
int ftruncate_fd(int fd, uint64_t size);

// Look up a path's metadata without opening it. Returns 0, or -ENOENT.
int stat_path(const char* path, Stat* out);

// The metadata of an open file, by fd — the same fields as stat_path, from the
// node the descriptor already holds. Returns 0, or -errno (EBADF; EINVAL for a
// console or a pipe, which are not files with a path).
int fstat_fd(int fd, Stat* out);

// How much room the filesystem holding 'path' has. A lookup, like stat: the
// search path applies. Returns 0, or -errno.
int statfs_path(const char* path, StatFs* out);

// The physical address and length of a file's bytes, for a zero-copy mapping.
// A lookup, like stat/open (the search path applies). Returns 0, or -errno
// (ENOENT; EROFS when the backend cannot promise the bytes stay put).
int map_file(const char* path, uint64_t* out_phys, uint64_t* out_size);

// fd table plumbing (implemented against the current task in sched.cpp)
void      fds_init_current();               // zero the table
FileHandle** fds_of_current();               // the current task's table
FileHandle*  fd_slot_get(int fd);
uint64_t     fd_size(int fd);                // bytes reported at open
int          fd_slot_alloc(Node* node);      // takes ownership of the reference
} // namespace VFS

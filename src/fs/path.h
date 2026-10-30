#pragma once
#include <stdint.h>

// --- Pure path logic ---
// Every function here is a function of its arguments: no mount table, no
// scheduler, no console, no VFS globals. That is deliberate. It is the one
// piece of the filesystem that most needs a table of cases, and keeping it free
// of kernel dependencies is what lets tools/test_path.cpp build it on the host
// (part of `ninja check`).
//
// The VFS owns the policy *around* these calls (the CWD, the program search
// path); this unit owns the mechanics (joining, normalising, splitting, mount
// prefixes) and the limits that decide what is refused rather than truncated.
namespace FsPath {

// The longest absolute path, including its terminating NUL, that the VFS will
// carry. It has to be comfortably more than NAME_MAX: a maximal 255-byte name
// still needs room for the directories it lives in.
constexpr int PATH_MAX = 512;

// The longest single name. ext2 permits 255 bytes, so 256 is the smallest value
// that can hold one; a name of NAME_MAX bytes is refused, one of NAME_MAX-1 is
// accepted.
constexpr int NAME_MAX = 256;

// The most components a canonical path can carry. ".." pops one, so the
// effective depth never exceeds this; a path that needs more is refused.
constexpr int MAX_COMPONENTS = 32;

// Does the path start at the root? (A relative path starts at the CWD.)
bool is_absolute(const char* path);

// Canonical form of 'path' in 'out': absolute, one leading slash, no trailing
// slash, repeated slashes collapsed, '.' dropped and '..' applied. False when
// the result would not fit 'out' or would need more than MAX_COMPONENTS.
bool normalize(const char* path, char* out, uint64_t out_max);

// '<dir>/<name>' in 'out'. False when it does not fit out_max: a joined path is
// never truncated, because a shortened one names a different file.
bool join(const char* dir, const char* name, char* out, uint64_t out_max);

// 'path' made absolute: used as it is when it already starts with '/',
// otherwise joined to 'cwd'. Deciding what the CWD is belongs to the caller.
bool absolute(const char* path, const char* cwd, char* out, uint64_t out_max);

// Can a component of 'len' bytes name an entry? (Strictly fewer than NAME_MAX.)
bool component_fits(uint64_t len);

// One component of a filesystem-relative path, written to 'out' and advancing
// *cursor past it. END at the end of the path (including trailing slashes);
// TOO_LONG when the component cannot be a name.
enum ComponentResult { COMPONENT_END, COMPONENT_OK, COMPONENT_TOO_LONG };
ComponentResult next_component(const char** cursor, char* out, uint64_t out_max);

// Parent and final name of a filesystem-relative path. EMPTY when there is no
// final name ("" or "/"); TOO_LONG when the name does not fit 'name_max' or the
// parent does not fit 'parent_max'.
enum SplitResult { SPLIT_OK, SPLIT_EMPTY, SPLIT_TOO_LONG };
SplitResult split(const char* rel, char* parent, uint64_t parent_max,
                  char* name, uint64_t name_max);

// Does the mount point 'mount' cover the canonical path 'canon'? Both are
// normalised absolute paths. A prefix counts only at a component boundary:
// "/bin" covers "/bin" and "/bin/ls.elf", never "/binary". The root mount "/"
// covers every absolute path.
bool mount_covers(const char* mount, const char* canon);

// Index of the longest mount covering 'canon', or -1 when none does. A later
// entry wins a tie, which only a duplicate mount point can produce.
int longest_mount(const char* canon, const char* const* mount_paths, int count);

} // namespace FsPath

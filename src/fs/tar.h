#pragma once
#include <stdint.h>

// Minimal ustar (POSIX tar) filesystem backed by a Limine module.
// The module is detected by its ".tar" path suffix.

namespace Tar {
    // Scan Limine modules for a .tar archive. Safe to call once at boot.
    void init();

    bool is_mounted();
    uint64_t count(); // number of entries in the archive

    // Fill 'buf' with entry names separated by '\n', NUL-terminated.
    // Respects 'max'. Used by the list_files syscall.
    void list_into(char* buf, uint64_t max);

    // Look up a member by name. On success returns true and points
    // 'out_data' at the file's bytes directly inside the archive
    // (no copy), with 'out_size' = byte length.
    bool read_file(const char* path, char** out_data, uint64_t* out_size);
}

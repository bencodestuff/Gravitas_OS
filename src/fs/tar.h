#pragma once
#include <stdint.h>

// Minimal ustar (POSIX tar) filesystem backed by a Limine module.
// The module is detected by its ".tar" path suffix.

namespace VFS { struct Filesystem; }

namespace Tar {
    // Scan Limine modules for a .tar archive. Safe to call once at boot.
    void init();

    // The read-only VFS backend over the mounted archive.
    VFS::Filesystem* vfs_instance();
}

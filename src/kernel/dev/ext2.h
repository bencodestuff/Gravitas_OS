#pragma once

#include <stdint.h>

// ext2. The filesystem on the disk (see kernel/dev/memblock.h and the real
// drives): a real on-disk format with real metadata, so the node layer has
// something to be faithful to.

namespace VFS { struct Filesystem; }

namespace Block { struct Device; }

namespace Ext2 {

// Mount 'dev' at 'root_ino' (2 is the filesystem root; another inode makes a
// directory inside it the root of this mount). 'writable' installs the write,
// truncate, create, unlink and rename operations; a read-only mount leaves them
// null so the VFS refuses writes with EROFS. The same device may be mounted more
// than once: geometry, free counts and the inode reference table are shared per
// device, so two mounts see one filesystem.
//
// Returns the backend, or nullptr if it is not an ext2 filesystem this code can
// handle (or a writable mount of one whose on-disk features the writer does not
// maintain). Logs one line on success.
VFS::Filesystem* mount(Block::Device* dev, bool writable = false,
                       uint32_t root_ino = 2);

} // namespace Ext2

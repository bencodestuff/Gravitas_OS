#pragma once

// ext2, read-only. The filesystem on the RAM disk (see kernel/dev/memblock.h):
// a real on-disk format with real metadata, so the node layer has something to
// be faithful to.

namespace VFS { struct Filesystem; }

namespace Block { struct Device; }

namespace Ext2 {

// Mount 'dev' read-only. Returns the backend, or nullptr if it is not an ext2
// filesystem (or one this reader cannot handle). Logs one line on success.
VFS::Filesystem* mount(Block::Device* dev);

} // namespace Ext2

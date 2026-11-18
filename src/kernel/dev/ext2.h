#pragma once

#include <stdint.h>

// ext2. The filesystem on the disk (see kernel/dev/memblock.h and the real
// drives): a real on-disk format with real metadata, so the node layer has
// something to be faithful to.

namespace VFS { struct Filesystem; }

namespace Block { struct Device; }

namespace Ext2 {

// The largest file this backend can describe: i_size is the 32-bit size field
// of a second-generation inode, and the on-disk block numbers are 32-bit too, so
// this one number is the limit for both. e2_truncate refuses a length above it;
// the write path has to refuse the same thing for the same reason, which is what
// write_past_limit is for.
constexpr uint64_t MAX_FILE_BYTES = 0xFFFFFFFFull;

// True when a write of 'count' bytes at 'offset' would leave the file longer
// than MAX_FILE_BYTES, so the end could not be recorded on disk. Exact at the
// edges rather than approximate: a write ending precisely on MAX_FILE_BYTES
// fits, a zero-length write fits anywhere, and neither comparison can overflow
// (offset is bounded before the subtraction).
constexpr bool write_past_limit(uint64_t offset, uint64_t count) {
    if (count == 0) return false;
    if (offset > MAX_FILE_BYTES) return true;
    return count > MAX_FILE_BYTES - offset;
}

// Under that cap the write path's block index cannot wrap: the farthest byte is
// MAX_FILE_BYTES, and even at the smallest block size ext2 defines (1024, the
// mount refuses anything narrower) the index stays far inside a uint32_t. Pinned
// as a compile-time fact so raising the cap cannot silently break the cast.
static_assert(MAX_FILE_BYTES / 1024 < 0xFFFFFFFFull,
              "a capped file's block index must fit in 32 bits");

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

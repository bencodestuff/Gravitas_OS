#pragma once

// Overlay: the writable root. A writable upper (ramfs) is layered over the
// read-only lower (tarfs, the image), so "/" can be written without touching
// the image:
//
//   * reads look in the upper first, then the lower;
//   * anything that writes happens in the upper, copying a lower file up first
//     when the write needs its contents;
//   * readdir is the union, upper entries shadowing lower ones;
//   * deleting a lower-only entry leaves a whiteout marker in the upper.
//
// It implements the VFS::Filesystem ops table, so nothing above it changes.

namespace VFS { struct Filesystem; }

namespace Overlay {
    void init(VFS::Filesystem* upper, VFS::Filesystem* lower);
    VFS::Filesystem* instance();
}

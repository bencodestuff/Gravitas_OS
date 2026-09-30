#pragma once

// RamFS: in-memory filesystem with create/write support.
// Implements the VFS::Filesystem ops table.

namespace VFS { struct Filesystem; }

namespace RamFS {
    void init();                    // create the empty root node
    VFS::Filesystem* instance();    // the ops table to mount()
}
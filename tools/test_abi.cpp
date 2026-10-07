// The kernel and the libc describe the same syscall records in two headers that
// cannot include each other: src/fs/vfs.h is the kernel's view, grv/syscall.h is
// the ABI's. They have to agree byte for byte, because LISTDIR fills the
// caller's array in place rather than converting it.
//
// Nothing else checks this. The static_assert in the dispatcher pins the
// kernel's *size*, which a field reorder would pass straight through — and that
// is not hypothetical: with the type and the inode swapped, every directory
// reported as a file, and the symptom was a missing '/' in a listing.
//
// This runs on the host, so it is part of the fast half (`ninja check`).
#include <cstddef>
#include <cstdio>

#include "fs/vfs.h"
// Quoted, and reached through -iquote, so the libc's include directory does not
// shadow the host's <stdio.h> and friends for the standard library this test
// itself uses.
#include "grv/syscall.h"

static int failures = 0;

static void same(const char* what, size_t kernel, size_t abi) {
    if (kernel != abi) {
        std::printf("FAIL: %s: kernel says %zu, the ABI says %zu\n",
                    what, kernel, abi);
        failures++;
    }
}

int main() {
    // The directory entry, field for field.
    same("sizeof(DirEntry)", sizeof(VFS::DirEntry), sizeof(struct grv_dirent));
    same("offsetof(name)", offsetof(VFS::DirEntry, name),
         offsetof(struct grv_dirent, name));
    same("offsetof(type)", offsetof(VFS::DirEntry, type),
         offsetof(struct grv_dirent, type));
    same("offsetof(ino)", offsetof(VFS::DirEntry, ino),
         offsetof(struct grv_dirent, ino));
    same("NAME_MAX vs name[]", VFS::NAME_MAX,
         sizeof(((struct grv_dirent*)0)->name));

    // grv_stat: the dispatcher writes it as five plain 64-bit fields in this
    // order. StatPkt in src/arch/x86_64/idt.cpp asserts the same numbers from
    // the kernel's side, so the two halves are pinned to one set of values.
    same("sizeof(grv_stat)", 40, sizeof(struct grv_stat));
    same("grv_stat.size", 0, offsetof(struct grv_stat, size));
    same("grv_stat.is_dir", 8, offsetof(struct grv_stat, is_dir));
    same("grv_stat.ino", 16, offsetof(struct grv_stat, ino));
    same("grv_stat.mode", 24, offsetof(struct grv_stat, mode));
    same("grv_stat.mtime", 32, offsetof(struct grv_stat, mtime));

    if (failures == 0) {
        std::printf("test_abi: the kernel and the ABI agree\n");
        return 0;
    }
    std::printf("test_abi: %d mismatch(es)\n", failures);
    return 1;
}

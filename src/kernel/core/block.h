#pragma once
#include <stdint.h>

// block — the disk abstraction.
//
// A filesystem should not care whether its bytes come from a RAM buffer, a
// Limine module or a real disk, so everything above this line talks in 512-byte
// sectors through a Device. The layer is deliberately thin: a registry and a
// contract, no cache (the cache worth having is a page cache, and that belongs
// with the filesystem that knows what its pages mean).
//
// Devices are registered at boot. The first is the "primary" disk — what a
// filesystem mounts when nothing says otherwise.

namespace Block {

constexpr uint64_t SECTOR = 512;

struct Device {
    const char* name;
    uint64_t sectors;               // size, in sectors

    // Transfer 'count' sectors starting at 'lba'. Return how many actually
    // moved. A request that runs past the end moves none: a caller asking past
    // the end is broken, and a short read would hide it rather than say so.
    uint64_t (*read)(void* priv, uint64_t lba, uint64_t count, void* buf);
    uint64_t (*write)(void* priv, uint64_t lba, uint64_t count, const void* buf);

    void* priv;
};

constexpr int MAX_DEVICES = 4;

// Register a device. False if the table is full or the device is malformed.
bool add(Device* dev);

int count();
Device* get(int i);
Device* primary();                  // the first registered, or nullptr

// Exercise a device: write a pattern, read it back, check the bounds, and put
// back exactly what was there (it may hold a filesystem image). Returns whether
// the device behaved; prints nothing.
bool self_test(Device* dev);

} // namespace Block

#pragma once
#include <stdint.h>

// block — the disk abstraction.
//
// A filesystem should not care whether its bytes come from a RAM buffer, a
// Limine module or a real disk, so everything above this line talks in 512-byte
// sectors through a Device. The layer is deliberately thin: a registry and a
// contract, no cache. The cache worth having is a page cache and it lives
// directly above this one, in BlockCache; a filesystem that wants cached
// transfers should go through that, not add state here.
//
// Devices are registered at boot. There is no "primary" here on purpose: which
// disk holds the data is a boot policy decision, so the caller keeps the device
// it created rather than asking the layer to guess.

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

    // Push the device's own write cache to the medium. nullptr when the device
    // has no cache to flush (a RAM disk) or the driver has nothing to say, in
    // which case a completed write is already as durable as it can be.
    bool (*flush)(void* priv);

    void* priv;
};

constexpr int MAX_DEVICES = 4;

// Register a device. False if the table is full or the device is malformed.
bool add(Device* dev);

// Exercise a device: write a pattern, read it back, check the bounds, and put
// back exactly what was there (it may hold a filesystem image). Returns whether
// the device behaved; prints nothing.
bool self_test(Device* dev);

// Flush a device's write cache. True when the device has nothing to flush or
// the flush succeeded; false when it has a cache and the flush failed.
bool flush(Device* dev);

} // namespace Block

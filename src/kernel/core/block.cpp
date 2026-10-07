#include "kernel/core/block.h"

// See block.h. The registry is a small fixed table: this system has one disk
// and the room for a couple more (a second one, a CD, a USB stick).

namespace Block {

static Device* devices[MAX_DEVICES];
static int ndevices = 0;

bool add(Device* dev) {
    if (dev == nullptr || dev->read == nullptr || dev->write == nullptr)
        return false;
    if (dev->sectors == 0 || ndevices >= MAX_DEVICES) return false;
    devices[ndevices++] = dev;
    return true;
}

int count() { return ndevices; }

Device* get(int i) {
    return (i >= 0 && i < ndevices) ? devices[i] : nullptr;
}

Device* primary() { return ndevices > 0 ? devices[0] : nullptr; }

bool self_test(Device* dev) {
    if (dev == nullptr || dev->sectors < 8) return false;

    // Two sectors in the middle, and what was there, so the disk is left
    // exactly as it was found.
    const uint64_t lba = dev->sectors / 2;
    uint8_t saved[SECTOR * 2];
    uint8_t pattern[SECTOR * 2];
    uint8_t back[SECTOR * 2];

    if (dev->read(dev->priv, lba, 2, saved) != 2) return false;
    for (uint64_t i = 0; i < sizeof(pattern); i++)
        pattern[i] = (uint8_t)(i * 7 + 3);

    if (dev->write(dev->priv, lba, 2, pattern) != 2) return false;
    if (dev->read(dev->priv, lba, 2, back) != 2) return false;
    for (uint64_t i = 0; i < sizeof(back); i++)
        if (back[i] != pattern[i]) return false;

    // Past the end: nothing moves, in either direction. (A request that starts
    // inside and ends outside is refused whole, not trimmed.)
    if (dev->read(dev->priv, dev->sectors, 1, back) != 0) return false;
    if (dev->write(dev->priv, dev->sectors, 1, pattern) != 0) return false;
    if (dev->read(dev->priv, dev->sectors - 1, 2, back) != 0) return false;

    return dev->write(dev->priv, lba, 2, saved) == 2;
}

} // namespace Block

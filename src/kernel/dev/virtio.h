#pragma once
#include "kernel/core/block.h"

// virtio-blk, the legacy interface.
//
// QEMU's virtio-blk-pci is a modern (1.0) device by default: its registers sit
// behind PCI capabilities and memory BARs. The suite asks for the legacy device
// instead (`disable-modern=on`, see tests/harness.py), which is one I/O BAR
// with a fixed register layout and a single virtqueue. That is the same amount
// of driver as the ATA one, and the point of this step is the *Block* interface
// behind the hardware, not the 1.0 feature negotiation.
//
// Legacy virtio is a split virtqueue in guest memory: a descriptor table, an
// available ring the driver appends to, and a used ring the device appends to.
// A block request is a three-descriptor chain (header, data, status) and the
// driver polls the used ring, so the device never needs to interrupt.
//
// DMA addresses are physical, and the Block contract allows any buffer (the
// self-test hands over the kernel stack). The driver therefore stages each
// transfer through a small buffer in the direct map, so the caller's address
// never has to be physical.

namespace Virtio {

// Probe PCI for virtio-blk devices. Registers a Block device for each found and
// returns the first, or nullptr when there is none.
Block::Device* init();

int count();
Block::Device* device(int i);   // i in [0, count())

} // namespace Virtio

#pragma once
#include "kernel/core/block.h"

// memblock — a disk in RAM.
//
// The development substrate: filesystem code gets a real Block device with no
// hardware behind it, so it can be written and tested before any driver exists.
// An optional image seeds it — a filesystem made on the host — and the sectors
// then belong to the guest: writes land in the copy, so the image is never
// touched and a reboot starts clean.

namespace MemBlock {

// A disk of 'sectors' 512-byte sectors, seeded from 'image' (which must fit)
// when it is not null. Returns the device, or nullptr.
Block::Device* create(const char* name, uint64_t sectors,
                      const void* image, uint64_t image_bytes);

} // namespace MemBlock

#pragma once

// ATA PIO. The classic IDE task file on the legacy I/O ports: the CPU moves
// every byte through the data register, which is slow and simple and needs no
// PCI layer, no DMA descriptors and no interrupts.
//
// The controller is a real one only when the machine has it. On QEMU, q35's
// built-in SATA is AHCI and exposes no legacy ports at all; the suite therefore
// adds a `piix4-ide` device (see tests/harness.py). No drive, no device, and
// the caller falls back to the RAM disk.

namespace Block { struct Device; }

namespace Ata {

// Probe the primary channel's master and slave. Registers a Block device for
// each drive found and returns the first, or nullptr when there is none.
//
// The first drive is the one a caller should use as data. The rest are exposed
// so the caller can decide what they are: a test machine attaches a scratch
// drive as the slave, which is the only way to exercise the write path without
// writing to the data disk.
Block::Device* init();

int count();
Block::Device* device(int i);   // i in [0, count())

} // namespace Ata

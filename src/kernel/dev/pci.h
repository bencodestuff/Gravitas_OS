#pragma once
#include <stdint.h>

// pci — configuration space over the legacy 0xCF8/0xCFC port pair.
//
// This is the smallest PCI that finds a device: enough to walk bus 0, read a
// device's identity and take its base address registers. There is no MSI, no
// capability list walking, no bus-bridge recursion and no interrupt routing
// here, because nothing this system drives needs them (the one PCI driver,
// virtio-blk, polls its queue and never raises an interrupt).
//
// The 0xCF8/0xCFC mechanism is the old one; it only reaches bus 0 unless a
// host-bridge register is programmed. QEMU puts every device this kernel cares
// about on bus 0, so a scan of that bus is the whole of it.

namespace Pci {

struct Address {
    uint8_t bus;
    uint8_t slot;
    uint8_t func;
};

// Configuration-space reads. 'off' is a byte offset into the 256-byte header;
// the 32-bit access underneath is aligned down and the result shifted out.
uint32_t read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
uint16_t read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
uint8_t  read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);

// Every device on bus 0 whose vendor and device IDs match, written to 'out'
// (at most 'max'), returning how many. A 'device' of 0xFFFF matches any device
// from that vendor. The scan is in slot/function order.
int find_all(uint16_t vendor, uint16_t device, Address* out, int max);

// The first such device, or false when there is none.
bool find(uint16_t vendor, uint16_t device, Address* out);

// Base address register 'index' (0..5) as raw config bits: bit 0 says I/O vs
// memory, the rest is the address or the size probe. The caller interprets it.
uint32_t bar(const Address& a, int index);

} // namespace Pci

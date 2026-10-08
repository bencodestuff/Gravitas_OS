#include "kernel/dev/pci.h"
#include "kernel/io.h"

namespace Pci {

// The configuration address register: enable bit 31, then bus/device/function
// and the aligned register offset in bits 7-2.
constexpr uint16_t CONFIG_ADDRESS = 0xCF8;
constexpr uint16_t CONFIG_DATA    = 0xCFC;

static uint32_t address(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    return 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
           ((uint32_t)func << 8) | (off & 0xFC);
}

uint32_t read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    outl(CONFIG_ADDRESS, address(bus, slot, func, off));
    return inl(CONFIG_DATA);
}

uint16_t read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t v = read32(bus, slot, func, off & 0xFC);
    return (uint16_t)(v >> ((off & 2) * 8));
}

uint8_t read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t v = read32(bus, slot, func, off & 0xFC);
    return (uint8_t)(v >> ((off & 3) * 8));
}

uint32_t bar(const Address& a, int index) {
    return read32(a.bus, a.slot, a.func, (uint8_t)(0x10 + index * 4));
}

int find_all(uint16_t vendor, uint16_t device, Address* out, int max) {
    int found = 0;
    // Bus 0 only, every slot and function: a device that is not there reads
    // back 0xFFFF for its vendor, so this needs no topology knowledge.
    for (uint8_t slot = 0; slot < 32 && found < max; slot++) {
        for (uint8_t func = 0; func < 8 && found < max; func++) {
            uint16_t v = read16(0, slot, func, 0x00);
            if (v == 0xFFFF) {
                // No device at function 0 means no device at this slot at all;
                // a real single-function device just reads 0xFFFF on 1..7.
                if (func == 0) break;
                continue;
            }
            if (v != vendor) continue;
            uint16_t d = read16(0, slot, func, 0x02);
            if (device != 0xFFFF && d != device) continue;
            out[found].bus = 0;
            out[found].slot = slot;
            out[found].func = func;
            found++;
        }
    }
    return found;
}

bool find(uint16_t vendor, uint16_t device, Address* out) {
    return find_all(vendor, device, out, 1) > 0;
}

} // namespace Pci

#pragma once
#include <stdint.h>

namespace GDT {
    // Initialize the GDT
    void init();

    // These are the offsets we will need later when writing Assembly
    // 0x00 = Null Descriptor
    // 0x08 = Kernel Code
    // 0x10 = Kernel Data
    // 0x18 = User Data
    // 0x20 = User Code
    // 0x28 = TSS
}
#include "gdt.h"

extern "C" void kprint(const char* str);

// This function exists in Assembly (we will write it next)
// It tells the CPU: "Reload your segment registers with our new GDT!"
extern "C" void load_gdt(uint64_t gdtr_ptr);

namespace GDT {

    // A GDT Entry is 8 bytes (64 bits)
    struct GDTEntry {
        uint16_t limit_low;
        uint16_t base_low;
        uint8_t  base_middle;
        uint8_t  access;
        uint8_t  granularity;
        uint8_t  base_high;
    } __attribute__((packed));

    // The GDT Register (GDTR) points to the GDT
    struct GDTR {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed));

    // We need 5 entries: Null, Kernel Code, Kernel Data, User Data, User Code
    // (TSS will come later in Level 4)
    static GDTEntry gdt[5];
    static GDTR gdtr;

    // Helper to build an entry
    static void set_entry(int index, uint8_t access, uint8_t flags) {
        gdt[index].base_low    = 0;
        gdt[index].base_middle = 0;
        gdt[index].base_high   = 0;
        gdt[index].limit_low   = 0;  // Ignored in 64-bit mode usually
        gdt[index].access      = access;
        gdt[index].granularity = (flags << 4) | 0x00; // 0x00 because limit is 0
    }

    void init() {
        kprint("[GDT] Initializing...\n");

        // 0. Null Descriptor (Index 0) - Must be zero
        set_entry(0, 0, 0);

        // 1. Kernel Code (Index 1 -> Offset 0x08)
        // Access: Present(1) | Ring0(00) | Code/Data(1) | Exec(1) | Readable(1) = 0x9A
        // Flags: LongMode(1) = 0x2
        set_entry(1, 0x9A, 0x02);

        // 2. Kernel Data (Index 2 -> Offset 0x10)
        // Access: Present(1) | Ring0(00) | Code/Data(1) | Data(0) | Writable(1) = 0x92
        // Flags: 0
        set_entry(2, 0x92, 0x00);

        // 3. User Data (Index 3 -> Offset 0x18)
        // Access: Present(1) | Ring3(11) | Code/Data(1) | Data(0) | Writable(1) = 0xF2
        set_entry(3, 0xF2, 0x00);

        // 4. User Code (Index 4 -> Offset 0x20)
        // Access: Present(1) | Ring3(11) | Code/Data(1) | Exec(1) | Readable(1) = 0xFA
        // Flags: LongMode(1) = 0x2
        set_entry(4, 0xFA, 0x02);

        // Load the GDTR
        gdtr.limit = sizeof(gdt) - 1;
        gdtr.base  = (uint64_t)&gdt;

        kprint("[GDT] Loading new GDT...\n");
        load_gdt((uint64_t)&gdtr);
        kprint("[GDT] Loaded.\n");
    }
}
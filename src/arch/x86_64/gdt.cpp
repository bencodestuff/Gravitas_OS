#include "gdt.h"

extern "C" void kprint(const char* str);
extern "C" void load_gdt(uint64_t gdtr_ptr);

namespace GDT {

    // TSS defined in gdt.h; instantiate here so other files can reference it.
    // REMOVED 'static' so sched.cpp can see it later!
    TSS tss;

    struct GDTEntry {
        uint16_t limit_low;
        uint16_t base_low;
        uint8_t  base_middle;
        uint8_t  access;
        uint8_t  granularity;
        uint8_t  base_high;
    } __attribute__((packed));

    struct TSSEntry {
        uint16_t length;
        uint16_t base_low;
        uint8_t  base_mid;
        uint8_t  flags1;
        uint8_t  flags2;
        uint8_t  base_high;
        uint32_t base_upper;
        uint32_t reserved;
    } __attribute__((packed));

    // The Unified GDT Table (5 Standard Entries + 1 TSS System Entry)
    static struct {
        GDTEntry entries[5]; 
        TSSEntry tss_entry;
    } gdt_table;

    struct GDTR {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed));

    static GDTR gdtr;

    // Helper to write into the MAIN table
    static void set_entry(int index, uint8_t access, uint8_t flags) {
        gdt_table.entries[index].base_low    = 0;
        gdt_table.entries[index].base_middle = 0;
        gdt_table.entries[index].base_high   = 0;
        gdt_table.entries[index].limit_low   = 0;
        gdt_table.entries[index].access      = access;
        gdt_table.entries[index].granularity = (flags << 4) | 0x00;
    }

    void init() {
        kprint("[GDT] Initializing...\n");

        // 1. Setup TSS Default Values
        // We zero it out first to be safe
        for (uint64_t* p = (uint64_t*)&tss; p < (uint64_t*)(&tss + 1); p++) *p = 0;
        
        tss.rsp0 = 0; // Scheduler will update this
        tss.iomap_base = sizeof(TSS); 

        // Provide an IST (Interrupt Stack Table) entry for handling interrupts
        // that occur while running in user mode. This stack lives in the kernel
        // static area and will be used when IDT entries specify IST=1.
        static uint8_t irq_stack[4096];
        // IST index 1 corresponds to tss.ist[0]
        tss.ist[0] = (uint64_t)(&irq_stack[0] + sizeof(irq_stack));

        // 2. Setup Standard GDT Entries
        set_entry(0, 0, 0);       // Null
        set_entry(1, 0x9A, 0x02); // Kernel Code
        set_entry(2, 0x92, 0x00); // Kernel Data
        set_entry(3, 0xF2, 0x00); // User Data
        set_entry(4, 0xFA, 0x02); // User Code

        // 3. Setup TSS Entry manually (It's special)
        uint64_t base = (uint64_t)&tss;
        uint64_t limit = sizeof(TSS) - 1;

        gdt_table.tss_entry.length      = limit;
        gdt_table.tss_entry.base_low    = base & 0xFFFF;
        gdt_table.tss_entry.base_mid    = (base >> 16) & 0xFF;
        gdt_table.tss_entry.flags1      = 0x89; // Present | TSS Available
        gdt_table.tss_entry.flags2      = 0x00; 
        gdt_table.tss_entry.base_high   = (base >> 24) & 0xFF;
        gdt_table.tss_entry.base_upper  = base >> 32;
        gdt_table.tss_entry.reserved    = 0;

        // 4. Load the GDT
        // Make sure we point to gdt_table, NOT a local array!
        gdtr.limit = sizeof(gdt_table) - 1;
        gdtr.base  = (uint64_t)&gdt_table;

        kprint("[GDT] Loading new GDT...\n");
        load_gdt((uint64_t)&gdtr);

        // 5. Load the Task Register (LTR)
        // Offset 0x28 = Index 5 * 8 bytes
        asm volatile("ltr %%ax" :: "a"(0x28));

        kprint("[GDT] TSS Loaded.\n");
    }
}
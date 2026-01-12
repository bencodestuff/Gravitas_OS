#include <stdint.h>

namespace GDT {
    // Full TSS definition so other translation units can access fields like `rsp0`.
    struct TSS {
        uint32_t reserved0;
        uint64_t rsp0;
        uint64_t rsp1;
        uint64_t rsp2;
        uint64_t reserved1;
        uint64_t ist[7];
        uint64_t reserved2;
        uint16_t reserved3;
        uint16_t iomap_base;
    } __attribute__((packed));

    // Allow other files to see the TSS instance
    extern TSS tss;

    void init();
}
#pragma once
#include <stdint.h> 

// ELF Magic Numbers
#define ELF_MAGIC 0x464C457F // 0x7F, 'E', 'L', 'F' in little endian

// File Types
#define PT_LOAD 1  // Segment must be loaded into memory

namespace ELF {

    // 1. The File Header (Located at the very start of the file)
    struct Elf64_Ehdr {
        uint8_t  e_ident[16]; // Magic number and info
        uint16_t e_type;      // 2 = Executable
        uint16_t e_machine;   // 0x3E = x86-64
        uint32_t e_version;
        uint64_t e_entry;     // The memory address where execution starts!
        uint64_t e_phoff;     // Start of the Program Header Table
        uint64_t e_shoff;     // Start of Section Header Table
        uint32_t e_flags;
        uint16_t e_ehsize;
        uint16_t e_phentsize; // Size of one Program Header entry
        uint16_t e_phnum;     // How many Program Headers?
        uint16_t e_shentsize;
        uint16_t e_shnum;
        uint16_t e_shstrndx;
    };

    // 2. The Program Header (Describes a segment to load)
    struct Elf64_Phdr {
        uint32_t p_type;   // 1 = LOAD (We only care about these)
        uint32_t p_flags;  // Read/Write/Execute
        uint64_t p_offset; // Where in the file is the data?
        uint64_t p_vaddr;  // Where in memory should we put it?
        uint64_t p_paddr;  // (Ignored)
        uint64_t p_filesz; // How many bytes in the file?
        uint64_t p_memsz;  // How many bytes in memory? (Can be larger if BSS)
        uint64_t p_align;
    };

    // Function to load a file
    // Returns the Entry Point address (or 0 on failure)
    uint64_t load_elf(uint8_t* file_data, uint64_t file_size);

}
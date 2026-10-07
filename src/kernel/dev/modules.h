#pragma once
#include <stdint.h>

// Limine modules — the files the bootloader hands the kernel, listed as
// module_path lines in limine.conf. One request, in one place, so the tar image
// and the disk image do not each keep their own copy of it.

namespace Modules {

struct Module {
    const void* data;
    uint64_t    size;
    const char* path;    // as named in limine.conf
};

// The first module whose path ends with 'suffix' (e.g. ".tar"). False when
// there is none, or when the bootloader gave us nothing.
bool find(const char* suffix, Module* out);

uint64_t count();

} // namespace Modules

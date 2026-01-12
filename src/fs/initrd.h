#pragma once
#include <stdint.h>

namespace InitRD {
    void init();
    // Helper to read a file's content
    char* read_file(const char* path);
}
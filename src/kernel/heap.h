#pragma once
#include <stddef.h>
#include <stdint.h>

namespace Heap {
    void init(void* start_addr, size_t size);
    void* malloc(size_t size);
    void free(void* ptr);
}
#include <limine.h>
#include "kernel/dev/modules.h"

// The request itself. Limine finds it in .requests and fills the response in
// before the kernel runs, so everything here is readable from the first line of
// main().

__attribute__((used, section(".requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0,
    .response = nullptr,
    .internal_module_count = 0,
    .internal_modules = nullptr,
};

namespace Modules {

static bool ends_with(const char* s, const char* suffix) {
    const char* p = s;
    while (*p) p++;
    const char* q = suffix;
    while (*q) q++;
    if (q - suffix > p - s) return false;
    while (q > suffix) {
        if (*--p != *--q) return false;
    }
    return true;
}

bool find(const char* suffix, Module* out) {
    if (module_request.response == nullptr) return false;
    for (uint64_t i = 0; i < module_request.response->module_count; i++) {
        struct limine_file* f = module_request.response->modules[i];
        if (f->path == nullptr || !ends_with(f->path, suffix)) continue;
        if (out) {
            out->data = (const void*)f->address;
            out->size = f->size;
            out->path = f->path;
        }
        return true;
    }
    return false;
}

uint64_t count() {
    if (module_request.response == nullptr) return 0;
    return module_request.response->module_count;
}

} // namespace Modules

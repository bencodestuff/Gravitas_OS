#include <limine.h>
#include "initrd.h"
#include "../drivers/framebuffer.h"

__attribute__((used, section(".requests")))
volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0
};


namespace InitRD {
    // ... helper string functions ...
    
    void init() {
         if (module_request.response == nullptr) {
             kprint("[InitRD] No modules found.\n");
             return;
         }
         kprint("[InitRD] Modules loaded.\n");
    }
    
    char* read_file(const char* path) {
        // ... logic to loop through modules and find matching path ...
        return nullptr; // if not found
    }
}
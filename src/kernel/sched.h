#pragma once
#include <stdint.h>

namespace Sched {
    void init();
    void create_task(void(*entry_point)());
    void yield(); // Voluntarily give up CPU
}
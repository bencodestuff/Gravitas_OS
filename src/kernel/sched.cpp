#include "sched.h"
#include "heap.h"
#include "timer.h"
#include "arch/x86_64/gdt.h"

// Minimal freestanding strcpy implementation for the kernel (C linkage)
extern "C" char* strcpy(char* dest, const char* src) {
    char* ret = dest;
    while ((*dest++ = *src++) != '\0') {
        // copy
    }
    return ret;
}

extern "C" void kprint(const char* str);
extern "C" void switch_context(uint64_t* old_sp, uint64_t next_sp);



namespace Sched {
    enum TaskState {
        READY,
        SLEEPING
    };

    struct Task {
        uint64_t rsp;
        Task* next;
        int id;
        TaskState state;
        uint64_t wake_timestamp;
        char name[16];
        
        // --- MISSING FIELD ---
        uint64_t kstack_top; // We need this for the TSS!
    };

    Task* current_task = nullptr;
    Task* ready_queue = nullptr; // A simple linked list (Round Robin)
    int task_counter = 0;

    void sleep(uint64_t ms) {
        // 1. Calculate target time
        // Assuming 50Hz (20ms per tick)
        uint64_t ticks_needed = ms / 20;
        current_task->wake_timestamp = Timer::get_ticks() + ticks_needed;

        // 2. Mark as SLEEPING
        current_task->state = SLEEPING;

        // 3. Give up the CPU immediately
        yield();
    }

    void init() {
        // ... previous setup ...
        current_task = new Task;
        current_task->id = 0;
        current_task->state = READY;
        strcpy(current_task->name, "Kernel Shell");
        
        // --- THE FIX ---
        // Capture the current stack pointer so the TSS knows where to land
        // when we return to this task from User Mode.
        uint64_t current_rsp;
        asm volatile("mov %%rsp, %0" : "=r"(current_rsp));
        current_task->kstack_top = current_rsp; 
        // ----------------
        
        ready_queue = current_task;
        current_task->next = current_task;
        
        kprint("[Sched] Initialized.\n");
    }

    void create_task(void(*entry_point)()) {
        // 1. Allocate a new Task struct
        Task* new_task = new Task;
        new_task->id = ++task_counter;
        new_task->state = READY;
        new_task->wake_timestamp = 0;

        // 2. Allocate a Stack for this thread (4KB)
        uint64_t* stack_bottom = new uint64_t[512]; // 512 * 8 bytes = 4KB
        uint64_t* stack_top = stack_bottom + 512;

        new_task->kstack_top = (uint64_t)stack_top;

        // 3. Forge the Stack!
        // We need to match exactly what 'switch_context' expects to pop.
        // Order: RBX, RBP, R12, R13, R14, R15, RIP (Return Address)
        
        stack_top--; *stack_top = (uint64_t)entry_point; // RIP (where to start)
        stack_top--; *stack_top = 0; // R15
        stack_top--; *stack_top = 0; // R14
        stack_top--; *stack_top = 0; // R13
        stack_top--; *stack_top = 0; // R12
        stack_top--; *stack_top = 0; // RBP
        stack_top--; *stack_top = 0; // RBX

        new_task->rsp = (uint64_t)stack_top;

        // 4. Add to the circular linked list
        new_task->next = ready_queue->next;
        ready_queue->next = new_task;
        
        kprint("[Sched] Task Created.\n");
    }

    void yield() {

        if (current_task == nullptr) return;

        Task* old_task = current_task;
        Task* next_task = current_task->next;
        
        // --- LOOP UNTIL WE FIND A READY TASK ---
        // If everyone is sleeping, we just spin (idle loop)
        while (true) {
            // 1. Check if a sleeping task is done sleeping
            if (next_task->state == SLEEPING) {
                if (Timer::get_ticks() >= next_task->wake_timestamp) {
                    next_task->state = READY; // Wakey wakey!
                }
            }

            // 2. Is this task ready to run?
            if (next_task->state == READY) {
                break; // Found one!
            }

            // 3. Move to next
            next_task = next_task->next;
        }

        // Switch to the found task
        current_task = next_task;
        GDT::tss.rsp0 = current_task->kstack_top;
        switch_context(&old_task->rsp, next_task->rsp);
    }
}
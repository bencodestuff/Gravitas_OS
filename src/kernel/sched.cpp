#include "sched.h"
#include "heap.h"

extern "C" void kprint(const char* str);
extern "C" void switch_context(uint64_t* old_sp, uint64_t next_sp);

namespace Sched {

    struct Task {
        uint64_t rsp; // The single most important value: The Stack Pointer
        Task* next;
        int id;
    };

    Task* current_task = nullptr;
    Task* ready_queue = nullptr; // A simple linked list (Round Robin)
    int task_counter = 0;

    void init() {
        // 1. Create a "Shell" task for the currently running code
        // We don't need a stack because we are ALREADY running on it.
        // We just need a struct to hold our place when we switch away.
        
        current_task = new Task;
        current_task->id = 0;
        current_task->next = current_task; // Circular list (points to self)
        
        ready_queue = current_task;
        
        kprint("[Sched] Multithreading Initialized.\n");
    }

    void create_task(void(*entry_point)()) {
        // 1. Allocate a new Task struct
        Task* new_task = new Task;
        new_task->id = ++task_counter;

        // 2. Allocate a Stack for this thread (4KB)
        uint64_t* stack_bottom = new uint64_t[512]; // 512 * 8 bytes = 4KB
        uint64_t* stack_top = stack_bottom + 512;

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
        // Simple Round Robin Scheduler
        Task* old_task = current_task;
        Task* next_task = current_task->next;

        current_task = next_task;

        // MAGIC: Switch stacks
        switch_context(&old_task->rsp, next_task->rsp);
    }
}
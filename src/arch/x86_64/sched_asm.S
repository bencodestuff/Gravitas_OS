.intel_syntax noprefix

.global switch_context

// void switch_context(uint64_t* current_rsp_ptr, uint64_t next_rsp,
//                     void* old_fxsave_area, void* new_fxrstor_area);
// RDI = where to save the OLD stack pointer
// RSI = the NEW stack pointer to load
// RDX = OLD task's fxsave area (512 bytes, 64-byte aligned)
// RCX = NEW task's fxsave area
switch_context:
    // 1. Save the Callee-Saved registers of the OLD task
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    // 2. FPU/SSE state of the OLD task.
    // The Task struct's fxsave_area (512 bytes, 64-aligned) is at a fixed
    // offset from the Task* — the C++ side passes it in RDX.
    // fxsave needs 16-byte stack alignment: rsp is 16-aligned here after
    // the six 8-byte pushes (call frame aligned it).
    fxsave [rdx]

    // 2b. Save the current Stack Pointer (RSP) into the memory provided
    mov [rdi], rsp

    // 3. Load the NEW Stack Pointer
    // --- THE SWITCH HAPPENS HERE ---
    mov rsp, rsi

    // 3b. FPU/SSE state of the NEW task (area pointer passed in RCX)
    fxrstor [rcx]

    // 4. Restore the registers of the NEW task
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx

    // 5. Return to the new task!
    ret
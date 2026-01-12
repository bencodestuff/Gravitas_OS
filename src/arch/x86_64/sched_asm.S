.intel_syntax noprefix

.global switch_context

// void switch_context(uint64_t* current_rsp_ptr, uint64_t next_rsp);
// RDI = Pointer to where we save the OLD stack pointer
// RSI = The NEW stack pointer we want to load
switch_context:
    // 1. Save the Callee-Saved registers of the OLD task
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    // 2. Save the current Stack Pointer (RSP) into the memory location provided (RDI)
    mov [rdi], rsp

    // 3. Load the NEW Stack Pointer (RSP) from the argument (RSI)
    // --- THE SWITCH HAPPENS HERE ---
    mov rsp, rsi

    // 4. Restore the registers of the NEW task
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx

    // 5. Return to the new task!
    ret
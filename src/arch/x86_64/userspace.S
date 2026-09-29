.intel_syntax noprefix
.global enter_user_mode
.global user_entry_trampoline

// void enter_user_mode(uint64_t rip, uint64_t rsp);
// RDI = RIP (Entry Point)
// RSI = RSP (Stack Pointer)
enter_user_mode:
    cli
    // ... pushes (SS, RSP, RFLAGS, CS, RIP) ...
    push 0x1B
    push rsi
    push 0x202
    push 0x23
    push rdi

    // Setup Segments
    mov ax, 0x1B
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    
    // --- THE TEST: CLEAR REGISTERS ---
    xor rax, rax  // Set RAX to 0
    xor rbx, rbx
    xor rcx, rcx
    xor rdx, rdx
    // ---------------------------------

    iretq

// Scheduler entry point for userland tasks.
// The scheduler forges a switch_context stack where:
//   R12 = userland RIP
//   R13 = userland RSP
// This trampoline converts that into an iretq into ring 3.
user_entry_trampoline:
    mov rdi, r12
    mov rsi, r13
    jmp enter_user_mode
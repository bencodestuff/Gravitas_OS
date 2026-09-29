.intel_syntax noprefix
.global enter_user_mode
.global user_entry_trampoline

// void enter_user_mode(uint64_t rip, uint64_t rsp);
// RDI = RIP (Entry Point)
// RSI = RSP (Stack Pointer)
//
// R8/R9 (set by the trampoline) survive iretq — iretq only restores
// RIP/CS/RFLAGS/RSP/SS — and are moved into RDI/RSI just before jumping
// to userland, giving _start(uint64_t argc, char** argv) its arguments.
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

    // Pass process arguments into ring 3 (System V: RDI=argc, RSI=argv)
    mov rdi, r8
    mov rsi, r9

    iretq

// Scheduler entry point for userland tasks.
// The scheduler forges a switch_context stack where:
//   R12 = userland RIP
//   R13 = userland RSP
//   R14 = argc
//   R15 = argv
// This trampoline converts that into an iretq into ring 3.
user_entry_trampoline:
    mov r8,  r14    // argc
    mov r9,  r15    // argv
    mov rdi, r12
    mov rsi, r13
    jmp enter_user_mode
.intel_syntax noprefix

// We will call this C++ function later
.extern interrupt_dispatcher

// Export this symbol so C++ can see it
.global isr_stub_table

// --- MACROS to make life easier ---
// Some interrupts push an Error Code automatically, some don't.
// We want a consistent stack layout, so we push a dummy 0 for those that don't.

// Macro for interrupts with NO error code (push dummy 0)
.macro ISR_NOERRCODE num
    .global isr_\num
    isr_\num:
        push 0              // Push dummy error code
        push \num           // Push interrupt number
        jmp isr_common_stub
.endm

// Macro for interrupts WITH error code (CPU pushed it already)
.macro ISR_ERRCODE num
    .global isr_\num
    isr_\num:
        push \num           // Push interrupt number
        jmp isr_common_stub
.endm

// --- The Common Stub (The Trampoline) ---
isr_common_stub:
    // 1. Save all registers (Context Save)
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    // 2. Prepare C++ arguments
    // We pass the stack pointer (RSP) as the argument to the C++ function
    // The C++ function can then read the struct Registers* regs
    mov rdi, rsp

    // 3. Call the C++ Handler
    call interrupt_dispatcher

    // 4. Restore all registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    // 5. Clean up stack (pop error code and interrupt number)
    add rsp, 16 

    // 6. Return from Interrupt
    iretq

// --- The Table of ISRs ---
// We create an array of pointers to the ISRs above
// This lets C++ easily loop through them.
.section .data
.global isr_stub_table
isr_stub_table:
    .quad isr_0
    .quad isr_1
    .quad isr_2
    .quad isr_3
    .quad isr_4
    .quad isr_5
    .quad isr_6
    .quad isr_7
    .quad isr_8
    .quad isr_9
    .quad isr_10
    .quad isr_11
    .quad isr_12
    .quad isr_13
    .quad isr_14
    .quad isr_15
    .quad isr_16
    .quad isr_17
    .quad isr_18
    .quad isr_19
    .quad isr_20
    .quad isr_21
    .quad isr_22
    .quad isr_23
    .quad isr_24
    .quad isr_25
    .quad isr_26
    .quad isr_27
    .quad isr_28
    .quad isr_29
    .quad isr_30
    .quad isr_31
    .quad isr_32
    .quad isr_33
    .quad isr_34
    .quad isr_35
    .quad isr_36
    .quad isr_37
    .quad isr_38
    .quad isr_39
    .quad isr_40
    .quad isr_41
    .quad isr_42
    .quad isr_43
    .quad isr_44
    .quad isr_45
    .quad isr_46
    .quad isr_47

// --- Define the actual ISR labels ---
// Most exceptions (0-31)
.text
ISR_NOERRCODE 0
ISR_NOERRCODE 1
ISR_NOERRCODE 2
ISR_NOERRCODE 3
ISR_NOERRCODE 4
ISR_NOERRCODE 5
ISR_NOERRCODE 6
ISR_NOERRCODE 7
ISR_ERRCODE   8  // Double Fault
ISR_NOERRCODE 9
ISR_ERRCODE   10 // Invalid TSS
ISR_ERRCODE   11 // Segment Not Present
ISR_ERRCODE   12 // Stack Segment Fault
ISR_ERRCODE   13 // General Protection Fault (GPF)
ISR_ERRCODE   14 // Page Fault
ISR_NOERRCODE 15
ISR_NOERRCODE 16 // Floating Point Exception
ISR_ERRCODE   17 // Alignment Check
ISR_NOERRCODE 18
ISR_NOERRCODE 19
ISR_NOERRCODE 20
ISR_NOERRCODE 21
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28
ISR_NOERRCODE 29
ISR_ERRCODE   30 // Security Exception
ISR_NOERRCODE 31
// --- Hardware Interrupts (IRQ 0-15 mapped to 32-47) ---
ISR_NOERRCODE 32 // Timer
ISR_NOERRCODE 33 // Keyboard
ISR_NOERRCODE 34
ISR_NOERRCODE 35
ISR_NOERRCODE 36
ISR_NOERRCODE 37
ISR_NOERRCODE 38
ISR_NOERRCODE 39
ISR_NOERRCODE 40
ISR_NOERRCODE 41
ISR_NOERRCODE 42
ISR_NOERRCODE 43
ISR_NOERRCODE 44
ISR_NOERRCODE 45
ISR_NOERRCODE 46
ISR_NOERRCODE 47
ISR_NOERRCODE 128
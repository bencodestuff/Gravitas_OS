/* Tell Clang to use Intel Syntax */
.intel_syntax noprefix

.global load_gdt

load_gdt:
    /* RDI contains the pointer to our GDTR */
    lgdt [rdi]

    /* Push Code Segment (0x08) */
    push 0x08
    
    /* Push Return Address (Label .reload_cs) */
    lea rax, [rip + .reload_cs]
    push rax
    
    /* Far Return (Intel Syntax name: retfq) */
    retfq

.reload_cs:
    /* Reload Data Segments with 0x10 */
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    ret
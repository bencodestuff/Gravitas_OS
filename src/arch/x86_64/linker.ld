ENTRY(kmain)

PHDRS
{
    text    PT_LOAD    FLAGS((1 << 0) | (1 << 2)) ; /* Execute + Read */
    rodata  PT_LOAD    FLAGS((1 << 2)) ;            /* Read only */
    data    PT_LOAD    FLAGS((1 << 1) | (1 << 2)) ; /* Write + Read */
}

SECTIONS
{
    /* We want to be placed in the High Half (Upper memory) */
    . = 0xffffffff80000000;

    /* CODE SECTION */
    .text : {
        *(.text .text.*)
    } :text

    /* Move to next page boundary */
    . = ALIGN(0x1000);

    /* READ-ONLY DATA SECTION */
    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    /* Move to next page boundary */
    . = ALIGN(0x1000);

    /* READ-WRITE DATA SECTION */
    .data : {
        *(.data .data.*)

        /* Global constructors: kmain walks this before anything else runs.
           KEEP prevents gc-sections from ever dropping them; SORT keeps
           init priority ordering (.init_array.00101 before .init_array). */
        . = ALIGN(8);
        __init_array_start = .;
        KEEP(*(.init_array))
        KEEP(*(SORT(.init_array.*)))
        __init_array_end = .;

        /* Keep requests inside .data but distinct */
        . = ALIGN(0x1000);
        KEEP(*(.requests_start_marker))
        KEEP(*(.requests))
        KEEP(*(.requests_end_marker))
    } :data

    /* Move to next page boundary */
    . = ALIGN(0x1000);

    /* BSS (Zero-initialized data) */
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /DISCARD/ : {
        *(.eh_frame)
        *(.note .note.*)
    }
}

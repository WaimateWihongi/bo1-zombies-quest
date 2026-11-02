/* bo1_guest.elf: a static AArch64 ELF of ILP32 code, linked at BO1_GUEST_LINK_BASE (port/include/bo1_abi.h).
   The host loads it at any free 64 KB-aligned address below 4 GB, moving the pointer words listed
   in .guest_relocs (port/tools/guest_relocs.py). */
ENTRY(__guest_header)

SECTIONS
{
    . = 0x80000000;
    .guest_header : { KEEP(*(.guest_header)) }
    /* each segment on pages of its own (16 KB, the largest Android page): the loader sets
       protection page by page */
    . = ALIGN(0x4000);
    .text : { *(.text .text.*) }
    . = ALIGN(0x4000);
    .rodata : { *(.rodata .rodata.*) }
    . = ALIGN(0x10000);
    .data : { *(.data .data.*) }
    .guest_init : {
        __guest_init_start = .;
        KEEP(*(.guest_init))
        __guest_init_end = .;
    }
    .bss : { *(.bss .bss.*) *(COMMON) }
    __guest_image_end = .;
    /DISCARD/ : { *(.eh_frame) *(.comment) *(.note.*) *(.guest_fini) }
}

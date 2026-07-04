/* linker.ld
 *
 * Memory layout for the flat kernel binary GRUB loads.
 *
 * 1 MiB is the conventional load address for a Multiboot kernel - everything
 * below it is legacy BIOS/VGA territory that isn't safe to place code in.
 * .multiboot is pinned first so the header falls inside the first 8 KiB,
 * which is as far as GRUB scans looking for it.
 */

ENTRY(_start)

SECTIONS
{
    . = 1M;

    .text BLOCK(4K) : ALIGN(4K)
    {
        *(.multiboot)
        *(.text)
    }

    .rodata BLOCK(4K) : ALIGN(4K)
    {
        *(.rodata)
    }

    .data BLOCK(4K) : ALIGN(4K)
    {
        *(.data)
    }

    .bss BLOCK(4K) : ALIGN(4K)
    {
        *(COMMON)
        *(.bss)
    }

    /* First free physical byte past the image; the heap grows from here. */
    . = ALIGN(4K);
    _kernel_end = .;
}

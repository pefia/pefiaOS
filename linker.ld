/* linker.ld
 * -----------------------------------------------------------------------------
 * Tells the linker how to lay out the kernel in memory.
 *
 * GRUB loads us at the conventional 1 MiB mark (the first megabyte is full of
 * legacy BIOS / VGA memory and is not safe to load a kernel into). The
 * .multiboot section is forced first so the Multiboot header lands within the
 * first 8 KiB, which is what GRUB scans.
 * -----------------------------------------------------------------------------
 */

ENTRY(_start)            /* execution starts at _start in boot/boot.asm */

SECTIONS
{
    . = 1M;              /* place the kernel at physical address 0x00100000 */

    /* Code. .multiboot MUST come first so GRUB can find the header early. */
    .text BLOCK(4K) : ALIGN(4K)
    {
        *(.multiboot)
        *(.text)
    }

    /* Read-only data (string literals, const tables). */
    .rodata BLOCK(4K) : ALIGN(4K)
    {
        *(.rodata)
    }

    /* Initialized read/write data. */
    .data BLOCK(4K) : ALIGN(4K)
    {
        *(.data)
    }

    /* Zero-initialized data and the stack reservation from boot.asm. */
    .bss BLOCK(4K) : ALIGN(4K)
    {
        *(COMMON)
        *(.bss)
    }

    /* Marks the first free byte past the kernel image. The heap starts here. */
    . = ALIGN(4K);
    _kernel_end = .;
}

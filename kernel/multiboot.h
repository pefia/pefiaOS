/* kernel/multiboot.h
 * -----------------------------------------------------------------------------
 * Just enough of the Multiboot v1 info structure to find the framebuffer and
 * basic memory amounts. Field offsets must match the spec exactly.
 * -----------------------------------------------------------------------------
 */
#ifndef PEFIA_MULTIBOOT_H
#define PEFIA_MULTIBOOT_H

#include <stdint.h>

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002u  /* GRUB passes this in EAX */
#define MULTIBOOT_INFO_MEMORY      0x00000001u  /* flags bit 0: mem_* valid */
#define MULTIBOOT_INFO_MMAP        0x00000040u  /* flags bit 6: mmap_* valid */
#define MULTIBOOT_INFO_FRAMEBUFFER 0x00001000u  /* flags bit 12: fb_* valid */

#define MULTIBOOT_MEMORY_AVAILABLE 1            /* mmap entry type: usable RAM */

struct multiboot_info {
    uint32_t flags;
    uint32_t mem_lower;          /* KB below 1 MiB */
    uint32_t mem_upper;          /* KB above 1 MiB */
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;  /* bytes per scanline */
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;   /* 1 = direct RGB */
    /* For type 1: red_pos, red_size, green_pos, green_size, blue_pos, blue_size */
    uint8_t  color_info[6];
} __attribute__((packed));

/* One entry in the GRUB memory map (mmap_addr .. mmap_addr + mmap_length).
 * NOTE: `size` does NOT count itself, so the next entry is at
 * (uint8_t *)entry + entry->size + 4. */
struct multiboot_mmap_entry {
    uint32_t size;
    uint64_t addr;
    uint64_t len;
    uint32_t type;       /* 1 = available */
} __attribute__((packed));

#endif /* PEFIA_MULTIBOOT_H */
